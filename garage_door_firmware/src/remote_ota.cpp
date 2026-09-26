#include "remote_ota.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include "log_ship.h"
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>

#ifndef OTA_VERSION_URL
#define OTA_VERSION_URL "http://door.wzx.homes/ota/version"
#endif
#ifndef OTA_BIN_URL
#define OTA_BIN_URL "http://door.wzx.homes/ota/firmware.bin"
#endif
#ifndef OTA_HTTP_TIMEOUT_MS
#define OTA_HTTP_TIMEOUT_MS 8000
#endif

static ConfigStore* s_cfg = nullptr;
static OtaBusyFn s_busyFn = nullptr;
static bool s_force = false;
static bool s_active = false;
static bool s_done = false;
static char s_lastMsg[64] = "idle";

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

static bool httpGetStream(const String& host, uint16_t port, const String& path,
                          WiFiClient* client, long* contentLen) {
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) return false;
  if (!client->connect(addr, port, OTA_HTTP_TIMEOUT_MS)) return false;
  client->setTimeout(30000);  // 大固件读包慢，别被默认超时掐断
  String req;
  req.reserve(128);
  // HTTP/1.0：避免 chunked；无 Content-Length 时也能按连接关闭读完
  req += "GET ";
  req += path;
  req += " HTTP/1.0\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nConnection: close\r\n\r\n";
  if (client->print(req) != (int)req.length()) {
    client->stop();
    return false;
  }
  uint32_t start = millis();
  String head;
  while (client->connected() || client->available()) {
    if (millis() - start > OTA_HTTP_TIMEOUT_MS) break;
    while (client->available()) {
      char c = (char)client->read();
      head += c;
      if (head.indexOf("\r\n\r\n") >= 0) break;
      if (head.length() > 2048) {
        client->stop();
        return false;
      }
    }
    if (head.indexOf("\r\n\r\n") >= 0) break;
    delay(1);
  }
  int hdrEnd = head.indexOf("\r\n\r\n");
  if (hdrEnd < 0) {
    client->stop();
    return false;
  }
  String header = head.substring(0, hdrEnd);
  int sp1 = header.indexOf(' ');
  int sp2 = header.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return false;
  if (header.substring(sp1 + 1, sp2).toInt() != 200) {
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

// 完整性不过就 abort，绝不 set_boot。停 NFC/BT 由 s_busyFn 在写 flash 前完成。
static void doOta() {
  s_force = false;
  if (s_active || s_done) return;

  String host, vpath, bpathUrl = OTA_BIN_URL;
  uint16_t port = 80;
  parseHttpUrl(OTA_VERSION_URL, &host, &port, &vpath, "/ota/version");
  vpath = withId(vpath);

  WiFiClient client;
  long clen = -1;
  if (!httpGetStream(host, port, vpath, &client, &clen)) {
    setMsg("version fetch fail");
    logShipf("[OTA] version fetch fail id=%s", deviceId().c_str());
    return;
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
    return;
  }
  remoteVer.trim();
  parseJsonStr(body, "sha256", &remoteSha);
  remoteSha.trim();
  remoteSha.toLowerCase();
  if (remoteVer == FW_VERSION) {
    setMsg("up to date");
    s_done = true;
    logShipf("[OTA] up to date %s", FW_VERSION);
    return;
  }
  logShipf("[OTA] new %s -> %s sha=%s id=%s", FW_VERSION, remoteVer.c_str(),
           remoteSha.length() ? remoteSha.substring(0, 12).c_str() : "-",
           deviceId().c_str());

  s_active = true;
  // 写 flash 前停 NFC/Inquiry（busy 钩子），避免 I2C 弄脏镜像
  if (s_busyFn) s_busyFn(true);
  // 排空 http worker（拒新单+等在飞结束）：把它占的堆还回来，
  // 否则 Update.begin 内部 4KB malloc 在低堆下必失败
  bool httpIdle = httpPause(4000);
  logShipf("[OTA] http paused idle=%d heap=%u maxblk=%u", (int)httpIdle,
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  long fsize = -1;
  String bpath;
  parseHttpUrl(bpathUrl, &host, &port, &bpath, "/ota/firmware.bin");
  bpath = withId(bpath);
  if (!httpGetStream(host, port, bpath, &client, &fsize) || fsize == 0) {
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    setMsg("bin fetch fail");
    logShipf("[OTA] bin fetch fail id=%s", deviceId().c_str());
    return;
  }
  logShipf("[OTA] bin ok size=%ld heap=%u maxblk=%u", fsize,
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // 清掉可能残留的 Update 状态
  Update.abort();
  // 擦 2MB OTA 槽可能踩 loop WDT，先摘掉
  disableLoopWDT();
  bool began = Update.begin(fsize > 0 ? (size_t)fsize : UPDATE_SIZE_UNKNOWN);
  uint8_t err1 = Update.getError();  // 第一次的真实错误（abort 会覆盖）
  if (!began) {
    Update.abort();
    began = Update.begin(fsize > 0 ? (size_t)fsize : UPDATE_SIZE_UNKNOWN);
  }
  enableLoopWDT();
  if (!began) {
    client.stop();
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    setMsg("update begin fail");
    logShipf("[OTA] Update.begin fail err1=%u err2=%u heap=%u maxblk=%u",
             (unsigned)err1, (unsigned)Update.getError(),
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return;
  }

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
  start = millis();
  while ((client.connected() || client.available()) &&
         (fsize < 0 || (long)written < fsize)) {
    if (millis() - start > 120000UL) break;
    int n = client.available();
    if (n <= 0) {
      delay(1);
      continue;
    }
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    int r = client.read(buf, n);
    if (r <= 0) break;
    if (Update.write(buf, (size_t)r) != (size_t)r) {
      Update.abort();
      mbedtls_sha256_free(&sha);
      client.stop();
      s_active = false;
      if (s_busyFn) s_busyFn(false);
      httpResume();
      setMsg("write fail");
      logShipf("[OTA] write fail at %u err=%s", (unsigned)written,
               Update.errorString());
      return;
    }
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 3)
    mbedtls_sha256_update(&sha, buf, (size_t)r);
#else
    mbedtls_sha256_update_ret(&sha, buf, (size_t)r);
#endif
    written += (size_t)r;
    start = millis();
    // 1.9MB 边下边写会堵死 loop → 看门狗复位；必须让出
    yield();
    delay(0);
    esp_task_wdt_reset();
    if (written >= nextLogAt && fsize > 0) {
      logShipf("[OTA] write %u/%ld", (unsigned)written, fsize);
      nextLogAt += 0x20000;
    }
  }
  client.stop();

  // 半截镜像不能激活
  if (fsize > 0 && (long)written < fsize) {
    Update.abort();
    mbedtls_sha256_free(&sha);
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    setMsg("partial write");
    logShipf("[OTA] partial %u/%ld → abort", (unsigned)written, fsize);
    return;
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
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    setMsg("sha mismatch");
    logShipf("[OTA] sha mismatch got=%s want=%s wrote=%u", hex,
             remoteSha.c_str(), (unsigned)written);
    return;
  }

  disableLoopWDT();
  bool ended = Update.end(true);
  enableLoopWDT();
  if (!ended) {
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    httpResume();
    setMsg("end fail");
    logShipf("[OTA] Update.end fail err=%s wrote=%u/%ld sha=%s",
             Update.errorString(), (unsigned)written, fsize, hex);
    return;
  }
  setMsg("rebooting");
  logShipf("[OTA] OK bytes=%u sha=%s id=%s -> reboot", (unsigned)written, hex,
           deviceId().c_str());
  // 真正 POST 出去再重启，否则日志全丢
  logShipFlushNow();
  // 软重启前再收一次 NFC：避免踩在 InList 半截 → PN532 拉死 SCL
  if (s_busyFn) s_busyFn(true);
  delay(300);
  ESP.restart();
}

// 仅手动/指令触发：无定时自动检查，防止升到不想升的版本
void remoteOtaService(bool btBusy, bool wifiOk) {
  if (s_active || s_done || !wifiOk) return;
  if (!s_force) return;
  // update 令必须能插队，不因 Inquiry 一直饿死
  (void)btBusy;
  doOta();
}
