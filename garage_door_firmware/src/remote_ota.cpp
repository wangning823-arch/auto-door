#include "remote_ota.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include "log_ship.h"
#include "ble_tracker.h"
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

#ifndef OTA_VERSION_URL
#define OTA_VERSION_URL "http://door.wzx.homes/ota/version"
#endif
#ifndef OTA_BIN_URL
#define OTA_BIN_URL "http://door.wzx.homes/ota/firmware.bin"
#endif
#ifndef OTA_HTTP_TIMEOUT_MS
#define OTA_HTTP_TIMEOUT_MS 20000
#endif

static ConfigStore* s_cfg = nullptr;
static OtaBusyFn s_busyFn = nullptr;
static bool s_force = false;
static bool s_active = false;
static bool s_done = false;
static char s_lastMsg[64] = "idle";
static char s_httpWhy[48] = "";  // httpGetStream 最近一次失败原因
static bool s_radioDown = false;  // BT已停+省电已关：所有退出路径需重启恢复

// 开机预留的 4KB 连续 8BIT 堆：Update.begin 内部 malloc(4KB) 要的是 8BIT 池，
// getMaxAllocHeap 报的 INTERNAL 最大块（11252）malloc 用不了——dda0 实测
// max8=2420 → probe 必败。开机时堆干净，此时切一块放着，begin 前让出。
static void* s_otaRes4k = nullptr;

void remoteOtaHold4k() {
  if (s_otaRes4k) return;
  s_otaRes4k = malloc(4096);
  logShipf("[OTA] hold4k %s free=%u max8=%u",
           s_otaRes4k ? "ok" : "FAIL", (unsigned)ESP.getFreeHeap(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void setMsg(const char* m) {
  strncpy(s_lastMsg, m, sizeof(s_lastMsg) - 1);
  s_lastMsg[sizeof(s_lastMsg) - 1] = 0;
}

static void sha256ToHex(const uint8_t raw[32], char out[65]) {
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[(raw[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[raw[i] & 0xF];
  }
  out[64] = 0;
}

void remoteOtaBegin(ConfigStore* cfg) {
  s_cfg = cfg;
  s_force = false;
  s_done = false;
  s_active = false;
  logShipf("[OTA] manual-only (no auto check) url=%s", OTA_VERSION_URL);
}

void remoteOtaSetBusyHook(OtaBusyFn fn) { s_busyFn = fn; }

void remoteOtaCheckNow() {
  s_force = true;
  s_done = false;  // 手动 update 必须能再查
}

bool remoteOtaActive() { return s_active; }
const char* remoteOtaLastMsg() { return s_lastMsg; }

static void setHttpWhy(const char* why) {
  strncpy(s_httpWhy, why, sizeof(s_httpWhy) - 1);
  s_httpWhy[sizeof(s_httpWhy) - 1] = 0;
}

static bool httpGetStream(const String& host, uint16_t port, const String& path,
                          WiFiClient* client, long* contentLen) {
  setHttpWhy("");
  IPAddress addr;
  String req;
  req.reserve(128);
  // HTTP/1.0：避免 chunked；无 Content-Length 时也能按连接关闭读完
  req += "GET ";
  req += path;
  req += " HTTP/1.0\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nConnection: close\r\n\r\n";

  // DNS/connect/send 在 loop 里最坏 31s+20s+10s，而 loopTask 看门狗 5s 就咬：
  // 实测 dda0 两次 OTA 均死在起步（nginx 连 /ota/version 都没收到、随后 BOOT
  // 复位）。三者内部都有秒级上界，期间撤监控、结束后立刻补喂。
  // 等 DNS 锁放在撤看门狗之前：锁内每 50ms 喂狗，卡在锁上能被 TWT 救回；
  // 锁只包 hostByName（与 worker 的 httpExchange 互斥），不包 connect/send。
  if (!httpDnsLock(HTTP_DNS_LOCK_WAIT_LOOP_MS)) {
    setHttpWhy("dns lock busy");
    return false;
  }
  disableLoopWDT();
  bool dnsOk = WiFi.hostByName(host.c_str(), addr);
  httpDnsUnlock();
  bool conn = dnsOk && client->connect(addr, port, (int32_t)OTA_HTTP_TIMEOUT_MS);
  size_t sent = 0;
  if (conn) {
    client->setTimeout(30000);  // 大固件读包慢，别被默认超时掐断
    sent = client->print(req);
  }
  enableLoopWDT();
  esp_task_wdt_reset();
  if (!dnsOk) {
    setHttpWhy("dns fail");
    return false;
  }
  if (!conn) {
    setHttpWhy("connect fail");
    return false;
  }
  if (sent != (size_t)req.length()) {
    client->stop();
    setHttpWhy("req send fail");
    return false;
  }
  uint32_t start = millis();
  String head;
  // 逐字节读是刻意的：块读会把 \r\n\r\n 之后的 body 开头一并吞进 head
  // （version JSON / bin 正文前缀），调用方从 socket 再读就缺了一段
  while (client->connected() || client->available()) {
    if (millis() - start > OTA_HTTP_TIMEOUT_MS) break;
    while (client->available()) {
      char c = (char)client->read();
      head += c;
      if (head.indexOf("\r\n\r\n") >= 0) break;
      if (head.length() > 2048) {
        client->stop();
        setHttpWhy("hdr too long");
        return false;
      }
    }
    if (head.indexOf("\r\n\r\n") >= 0) break;
    delay(1);
    esp_task_wdt_reset();  // header 等待最长 20s，必须喂狗
  }
  int hdrEnd = head.indexOf("\r\n\r\n");
  if (hdrEnd < 0) {
    bool still = client->connected();
    client->stop();
    setHttpWhy(still ? "hdr timeout" : "conn closed before hdr");
    return false;
  }
  String header = head.substring(0, hdrEnd);
  int sp1 = header.indexOf(' ');
  int sp2 = header.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) {
    setHttpWhy("bad status line");
    client->stop();
    return false;
  }
  int code = header.substring(sp1 + 1, sp2).toInt();
  if (code != 200) {
    char w[24];
    snprintf(w, sizeof(w), "http %d", code);
    setHttpWhy(w);
    client->stop();
    return false;
  }
  *contentLen = -1;
  int cl = header.indexOf("Content-Length:");
  if (cl < 0) cl = header.indexOf("content-length:");
  if (cl >= 0) {
    *contentLen = header.substring(cl + 15).toInt();
  }
  return true;
}

// 带重试的流式 GET：header 阶段偶发超时（BT/PS 抖动）时多试几次
static bool fetchWithRetry(const String& host, uint16_t port,
                           const String& path, WiFiClient* client, long* clen,
                           int tries, const char* tag) {
  for (int i = 0; i < tries; i++) {
    if (httpGetStream(host, port, path, client, clen)) return true;
    logShipf("[OTA] %s fetch fail try=%d/%d why=%s", tag, i + 1, tries,
             s_httpWhy[0] ? s_httpWhy : "?");
    logShipFlushNow();
    client->stop();
    if (i + 1 < tries) {
      uint32_t t0 = millis();
      while (millis() - t0 < 400) {
        delay(1);
        esp_task_wdt_reset();
      }
    }
  }
  return false;
}

static bool parseJsonStr(const String& body, const char* key, String* out) {
  String pat = String("\"") + key + "\"";
  int i = body.indexOf(pat);
  if (i < 0) return false;
  int colon = body.indexOf(':', i + pat.length());
  if (colon < 0) return false;
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  *out = body.substring(q1 + 1, q2);
  return out->length() > 0;
}

static String withId(const String& path) {
  String p = path;
  p += (p.indexOf('?') >= 0 ? '&' : '?');
  p += "id=";
  p += deviceId();
  return p;
}

static void parseHttpUrl(const String& url, String* host, uint16_t* port,
                         String* path, const char* defaultPath) {
  *host = "door.wzx.homes";
  *port = 80;
  *path = defaultPath;
  if (!url.startsWith("http://")) return;
  String rest = url.substring(7);
  int slash = rest.indexOf('/');
  String hp = slash >= 0 ? rest.substring(0, slash) : rest;
  *path = slash >= 0 ? rest.substring(slash) : String(defaultPath);
  int c = hp.indexOf(':');
  if (c >= 0) {
    *host = hp.substring(0, c);
    *port = (uint16_t)atoi(hp.substring(c + 1).c_str());
  } else {
    *host = hp;
  }
}

// 取版本确认有更新 → 立即蓝牙下电（释放 ~88KB）→ 等 WiFi 链路稳。
// 用户方案：update 结束必重启，蓝牙留着没意义——先下电再 begin，
// 8BIT 池直接回到 ~110KB，Update.begin 的 4KB malloc 不用再和 tcpip
// 抢碎池里的微秒窗口（旧顺序 begin 在前：max8≈4084 被抢 → 6 连败）。
// 返回 true = 有更新且射频已就绪（s_radioDown=true，失败路径必须重启恢复）；
// 返回 false = 无更新/取版本失败（射频未动，doOta 直接恢复现场）。
static bool otaPrepare(String* shaOut) {
  String host, vpath;
  uint16_t port = 80;
  parseHttpUrl(OTA_VERSION_URL, &host, &port, &vpath, "/ota/version");
  vpath = withId(vpath);

  WiFiClient client;
  long clen = -1;
  if (!fetchWithRetry(host, port, vpath, &client, &clen, 2, "version")) {
    setMsg("version fetch fail");
    logShipf("[OTA] version fetch fail id=%s why=%s", deviceId().c_str(),
             s_httpWhy[0] ? s_httpWhy : "?");
    return false;
  }
  String body;
  body.reserve(512);
  uint32_t start = millis();
  while (client.connected() || client.available()) {
    if (millis() - start > 2000) break;
    while (client.available()) body += (char)client.read();
    delay(1);
    if (body.length() > 512) break;
  }
  client.stop();

  String remoteVer, remoteSha;
  if (!parseJsonStr(body, "version", &remoteVer)) {
    setMsg("no version");
    logShipf("[OTA] version.json missing version");
    return false;
  }
  remoteVer.trim();
  parseJsonStr(body, "sha256", &remoteSha);
  remoteSha.trim();
  remoteSha.toLowerCase();
  if (remoteVer == FW_VERSION) {
    setMsg("up to date");
    s_done = true;
    logShipf("[OTA] up to date %s", FW_VERSION);
    return false;
  }
  logShipf("[OTA] new %s -> %s sha=%s id=%s", FW_VERSION, remoteVer.c_str(),
           remoteSha.length() ? remoteSha.substring(0, 12).c_str() : "-",
           deviceId().c_str());

  // 版本确有更新，才动射频：先停 BT 再关省电（BT 开着关省电 → wifi 断言
  // abort 必崩）。关掉省电后 AP 不再小缓冲排队，大流下行不再溢出丢包。
  // BT 栈已拆，之后任何退出路径都靠 ESP.restart 恢复（doOta 收尾处理）
  bool btDown = btRadioPowerDown();
  if (btDown) {
    WiFi.setSleep(false);
    s_radioDown = true;
  }
  logShipf("[OTA] radio btStop=%d sleep=%d", (int)btDown,
           (int)WiFi.getSleep());
  // BT 下电会引发 WiFi 射频重配，刚断开的 socket 全部作废：等链路稳定
  // 再开下载连接，否则下到一半 read 出错 → partial
  {
    uint32_t tw = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - tw < 8000) {
      delay(100);
      esp_task_wdt_reset();
    }
    delay(1500);
    esp_task_wdt_reset();
    logShipf("[OTA] wifi settle st=%d rssi=%d", (int)WiFi.status(),
             (int)WiFi.RSSI());
  }
  if (shaOut) *shaOut = remoteSha;
  return true;
}

// 下载+校验+激活（版本已确认、射频已下电、Update.begin 已由 doOta 完成）。
// 完整性不过就 abort，绝不 set_boot。停 NFC/BT 由 s_busyFn 完成（见 doOta 包装）。
// 成功路径不返回（ESP.restart）；其余情况返回，由 doOta 统一恢复现场。
static void otaAttempt(const String& remoteSha) {
  String host, bpathUrl = OTA_BIN_URL;
  uint16_t port = 80;
  WiFiClient client;

  // 下载+校验+激活整段最多 3 轮：BT 下电瞬断、链路抖动都可能断流
  for (int round = 1; round <= 3; round++) {
    if (round > 1) {
      logShipf("[OTA] dl retry round=%d/3", round);
      delay(2000);
      esp_task_wdt_reset();
      Update.abort();
      disableLoopWDT();
      bool again = Update.begin(UPDATE_SIZE_UNKNOWN);
      enableLoopWDT();
      if (!again) {
        logShipf("[OTA] retry begin fail heap=%u maxblk=%u",
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        break;
      }
    }

    long fsize = -1;
    String bpath;
    parseHttpUrl(bpathUrl, &host, &port, &bpath, "/ota/firmware.bin");
    bpath = withId(bpath);
    if (!fetchWithRetry(host, port, bpath, &client, &fsize, 3, "bin") ||
        fsize == 0) {
      Update.abort();
      setMsg("bin fetch fail");
      logShipf("[OTA] bin fetch fail round=%d why=%s fsize=%ld", round,
               s_httpWhy[0] ? s_httpWhy : "?", fsize);
      continue;
    }
    logShipf("[OTA] bin ok size=%ld round=%d heap=%u maxblk=%u rssi=%d sleep=%d",
             fsize, round, (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap(), (int)WiFi.RSSI(),
             (int)WiFi.getSleep());

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 3)
    mbedtls_sha256_starts(&sha, 0);
#else
    mbedtls_sha256_starts_ret(&sha, 0);
#endif

    uint8_t buf[1024];
    size_t written = 0;
    size_t nextLogAt = 0x20000;
    uint32_t dlStart = millis();
    uint32_t lastReport = millis();
    bool writeAborted = false;
    while ((client.connected() || client.available()) &&
           (fsize < 0 || (long)written < fsize)) {
      if (millis() - dlStart > 120000UL) break;
      int n = client.available();
      if (n <= 0) {
        delay(1);
        esp_task_wdt_reset();
        // 停滞每 5s 直推一条到 VPS：远程就能看到"卡在哪、信号/睡眠状态"
        if (millis() - lastReport >= 5000) {
          logShipf(
              "[OTA] stall %u/%ld for=%ums t=%u avail=%d conn=%d rssi=%d "
              "sleep=%d heap=%u",
              (unsigned)written, fsize, (unsigned)(millis() - dlStart),
              (unsigned)millis(), client.available(), (int)client.connected(),
              (int)WiFi.RSSI(), (int)WiFi.getSleep(),
              (unsigned)ESP.getFreeHeap());
          // 下载期间严禁同步 flush：flush 的 WiFiClient::stop() 是不关 fd 的
          // 空壳，高频 flush 泄漏 socket，与下载连接在 lwIP 冲突 → 设备侧
          // FIN 断连（实测每次 flush 后 <1s 断；注释后一次跑完全程）。
          // 日志只入环，下载结束/重启前统一 flush
          // logShipFlushNow();
          lastReport = millis();
        }
        continue;
      }
      if (n > (int)sizeof(buf)) n = sizeof(buf);
      int r = client.read(buf, n);
      if (r <= 0) break;
      if (Update.write(buf, (size_t)r) != (size_t)r) {
        Update.abort();
        mbedtls_sha256_free(&sha);
        client.stop();
        setMsg("write fail");
        logShipf("[OTA] write fail at %u round=%d err=%s", (unsigned)written,
                 round, Update.errorString());
        writeAborted = true;
        break;
      }
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 3)
      mbedtls_sha256_update(&sha, buf, (size_t)r);
#else
      mbedtls_sha256_update_ret(&sha, buf, (size_t)r);
#endif
      written += (size_t)r;
      dlStart = millis();
      // 1.9MB 边下边写会堵死 loop → 看门狗复位；必须让出
      yield();
      delay(0);
      esp_task_wdt_reset();
      if (written >= nextLogAt && fsize > 0) {
        logShipf("[OTA] write %u/%ld", (unsigned)written, fsize);
        nextLogAt += 0x20000;
        // 下载中 flush 会断连（见 stall 分支注释），只入环
        // logShipFlushNow();
        lastReport = millis();
      } else if (millis() - lastReport >= 10000) {
        logShipf("[OTA] dl %u/%ld", (unsigned)written, fsize);
        // logShipFlushNow();  // 同上：下载中禁 flush
        lastReport = millis();
      }
    }
    client.stop();
    if (writeAborted) continue;

    if (fsize > 0 && (long)written < fsize) {
      Update.abort();
      mbedtls_sha256_free(&sha);
      setMsg("partial write");
      logShipf("[OTA] partial %u/%ld round=%d → retry", (unsigned)written,
               fsize, round);
      continue;
    }

    uint8_t raw[32];
    char hex[65];
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 3)
    mbedtls_sha256_finish(&sha, raw);
#else
    mbedtls_sha256_finish_ret(&sha, raw);
#endif
    mbedtls_sha256_free(&sha);
    sha256ToHex(raw, hex);

    if (remoteSha.length() == 64 && strcmp(hex, remoteSha.c_str()) != 0) {
      Update.abort();
      setMsg("sha mismatch");
      logShipf("[OTA] sha mismatch got=%s want=%s wrote=%u", hex,
               remoteSha.c_str(), (unsigned)written);
      continue;
    }

    disableLoopWDT();
    bool ended = Update.end(true);
    enableLoopWDT();
    if (!ended) {
      setMsg("end fail");
      logShipf("[OTA] Update.end fail err=%s wrote=%u/%ld round=%d sha=%s",
               Update.errorString(), (unsigned)written, fsize, round, hex);
      continue;
    }
    setMsg("rebooting");
    logShipf("[OTA] OK bytes=%u sha=%s id=%s -> reboot", (unsigned)written,
             hex, deviceId().c_str());
    // 真正 POST 出去再重启，否则日志全丢
    logShipFlushNow();
    if (s_busyFn) s_busyFn(true);
    delay(300);
    ESP.restart();
  }
  logShipf("[OTA] 3 rounds failed -> give up");
}

static void doOta() {
  s_force = false;
  if (s_active || s_done) return;
  s_active = true;
  // 取版本之前就让路：header 阶段同样会被 Inquiry/BLE 掐（8s 超时来源）
  if (s_busyFn) s_busyFn(true);
  // 排空 http worker（拒新单+等在飞结束）：把它占的堆还回来。
  // 必须看返回值：false = 还有在飞请求（可能正卡在它自己的 DNS/connect 上），
  // 此时带着"在飞未知"往下走，Update.begin 会和它抢堆、日志 flush 会和它
  // 抢 DNS 锁——dda0 实测这条路径挂死 13.5 分钟，只能断电。
  // 4s 覆盖常态（单 job 超时 2.5~4s），再给 12s 覆盖 worker 等蓝牙让路的
  // 最坏 15s 窗口；合计 16s 仍不空就放弃本轮（s_done 不置位，下次 update 可再来）。
  bool httpIdle = httpPause(4000);
  if (!httpIdle) httpIdle = httpPause(12000);
  logShipf("[OTA] begin idle=%d heap=%u maxblk=%u sleep=%d rssi=%d",
           (int)httpIdle, (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getMaxAllocHeap(), (int)WiFi.getSleep(),
           (int)WiFi.RSSI());
  if (!httpIdle) {
    logShipf("[OTA] http worker busy >16s -> abort round, retry on next update");
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();  // httpPause 失败会保持暂停，不恢复就永远发不出请求
    return;
  }
  // 此刻网络还正常：预解析日志服务器 IP，下载停滞期的实时日志走 IP 直连
  // （停滞时 DNS 可阻塞 >5s → loopTask TWT 崩溃）
  logShipResolve();

  // 用户方案：先取版本确认有更新 → 蓝牙下电释放 ~88KB → 再 begin。
  // 旧顺序 begin 在最前：8BIT 池碎在 max8≈4KB，4KB 预留还要和 tcpip 抢
  // 微秒窗口（实测 res4k 被吃 → 6 连败 err=0）。下电后池子回到 ~110KB，
  // begin 十拿九稳；hold4k 预留逻辑保留作兜底。
  // （本注释更新 = 触发新版本号，用于线上验证新时序：radio 应先于 begin。）
  String remoteSha;
  if (!otaPrepare(&remoteSha)) {
    // 无更新/取版本失败：射频未动 → 恢复现场即可
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    return;
  }
  bool began = false;
  uint8_t err1 = 0;
  disableLoopWDT();
  for (int t = 0; t < 6 && !began; t++) {
    if (t) {
      Update.abort();
      uint32_t tw = millis();
      while (millis() - tw < 300) {
        delay(10);
        esp_task_wdt_reset();
      }
    }
    // 让出开机预留的 4KB 整块给 begin 的 malloc：运行久后 8BIT 池碎成
    // max8<4KB（探针实锤 maxIn=11252 但 max8=2420），只有这块从干净堆
    // 切出的连续内存能救。抢在 tcpip 拆分前的微秒级窗口里完成 malloc
    if (s_otaRes4k) {
      free(s_otaRes4k);
      s_otaRes4k = nullptr;
    }
    began = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (t == 0) err1 = Update.getError();
    if (!began) {
      s_otaRes4k = malloc(4096);  // 抢回留作下一轮（被吃则后续裸试）
      logShipf(
          "[OTA] begin try=%d err=%u res4k=%d free=%u maxIn=%u max8=%u",
          t, (unsigned)Update.getError(), (int)(s_otaRes4k != nullptr),
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
  }
  enableLoopWDT();
  if (!began) {
    setMsg("update begin fail");
    logShipf("[OTA] Update.begin fail err1=%u err2=%u heap=%u maxblk=%u",
             (unsigned)err1, (unsigned)Update.getError(),
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    if (s_radioDown) {
      // 射频已在 otaPrepare 拆掉：不重启则蓝牙跟踪永久失效
      s_radioDown = false;
      logShipf("[OTA] begin fail -> reboot to restore BT");
      logShipFlushNow();
      delay(300);
      ESP.restart();
    }
    return;
  }

  otaAttempt(remoteSha);
  // 失败/已最新 → 恢复 NFC/Inquiry、省电、放行 http、允许下次触发
  //（成功路径不返回：ESP.restart）
  Update.abort();
  WiFi.setSleep(true);
  s_active = false;
  if (s_busyFn) s_busyFn(false);
  httpResume();
  if (!s_otaRes4k) s_otaRes4k = malloc(4096);  // 本轮没重启 → 重新压住 4KB
  if (s_radioDown) {
    // BT 栈已在 otaAttempt 里拆除，不重启则蓝牙跟踪永久失效
    s_radioDown = false;
    logShipf("[OTA] bt down -> reboot to restore");
    logShipFlushNow();
    delay(300);
    ESP.restart();
  }
}

// 仅手动/指令触发：无定时自动检查，防止升到不想升的版本
void remoteOtaService(bool btBusy, bool wifiOk) {
  if (s_active || s_done || !wifiOk) return;
  if (!s_force) return;
  // update 令必须能插队，不因 Inquiry 一直饿死
  (void)btBusy;
  doOta();
}
