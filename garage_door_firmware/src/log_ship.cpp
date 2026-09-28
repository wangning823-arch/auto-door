#include "log_ship.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef LOG_SHIP_URL
#define LOG_SHIP_URL "http://door.wzx.homes/dev/logs"
#endif
#ifndef LOG_SHIP_INTERVAL_MS
#define LOG_SHIP_INTERVAL_MS 30000UL
#endif
#ifndef LOG_SHIP_TIMEOUT_MS
#define LOG_SHIP_TIMEOUT_MS 4000
#endif
// 环形缓冲约 2.5KB：够攒半分钟关键日志，不占大块
#ifndef LOG_SHIP_RING_BYTES
#define LOG_SHIP_RING_BYTES 2560
#endif
// 单次快照上限（分片上传）：碎片堆里 8BIT max8 实测可低至 ~2.4KB，整环 2560B
// 拷进 String 会 OOM → body 变空 → 发出空 POST → 服务端静默丢弃而环已摘走
// → 内容永久丢失且每轮重复（20260927 dda0：20 次200 空包 0 落盘的死循环）。
// 1536 < 观测最小 max8(2292)，拷贝必成；成功后 1s 续发下一片
#ifndef LOG_SHIP_CHUNK
#define LOG_SHIP_CHUNK 1536
#endif
// 下限：宁可发小片也不发零片。dda0 实测 8BIT largest 最低 980，
// 96 字节片只需 4*96+256=640，仍留 340 余量。
#ifndef LOG_SHIP_CHUNK_MIN
#define LOG_SHIP_CHUNK_MIN 96
#endif

// 分配期的固定开销：4 份 String 各 ~16B 块头 + req 的 160B 请求头 = 256。
// 并发余量 CHURN_SLACK：测量 largest 与实际 4 次分配之间，wifi/NFC 任务可能
// 已经吃掉一部分（dda0 弱网时每天 80 次 forceStaReconnect + NFC 无休止
// auto-retry 持续搅动堆）。零余量公式 cap=(L-192)/4 会让 need 恰好等于 L，
// 测量值稍一变化就拷贝失败 → 提交前 return → 一包不出网且永不自愈
// （20260928 dda0：重启后 1-2 分钟有日志，之后 38 分钟零 POST）。
#define LOG_SHIP_OVERHEAD 256
#define LOG_SHIP_CHURN_SLACK 512

// 按当前 8BIT 最大连续块给分片封顶。
// 固定 1536 在碎片堆里会 reserve 失败 → chunk=0 → 一包不发，积压永远抽不干。
//
// 一次发送期间**跨任务**同时存活多份拷贝：
//   loop 任务  : body、s_snap
//   worker 任务: HttpJob.body、req（req 还要 160B 请求头）
// 故 4*cap + OVERHEAD + CHURN_SLACK <= largest。
static size_t safeChunk() {
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t floor_need = LOG_SHIP_OVERHEAD + LOG_SHIP_CHURN_SLACK;
  if (largest <= floor_need) return 0;  // 连最小片的余量都保不住，下轮再试
  size_t cap = (largest - floor_need) / 4;
  if (cap > LOG_SHIP_CHUNK) cap = LOG_SHIP_CHUNK;
  if (cap < LOG_SHIP_CHUNK_MIN) cap = LOG_SHIP_CHUNK_MIN;
  return cap;
}

// ===== 发送阻塞点诊断（打进 status，远程即可看到卡在哪一步）=====
// why: 0=未尝试 1=在飞等待 2=wifi断 3=未到点 4=safeChunk=0(堆太碎)
//      5=环空(正常) 6=body拷贝失败 7=snap拷贝失败 8=submit失败 9=已发出
// largest8/cap/pend: 上次尝试时的 8BIT 最大块、算出的分片上限、环内积压
static volatile uint8_t s_why = 0;
static volatile uint32_t s_largest8 = 0;
static volatile uint32_t s_cap = 0;
static volatile uint32_t s_attempt = 0;  // 尝试次数（>0 说明确实走到发送流程）

static char s_ring[LOG_SHIP_RING_BYTES];
static size_t s_len = 0;  // 有效字节，紧凑存放
static uint32_t s_nextMs = 0;
static int s_failStreak = 0;
static bool s_inFlight = false;      // 快照已提交、结果未收
static String s_snap;                 // 在飞的请求快照（失败时塞回）
static SemaphoreHandle_t s_mtx = nullptr;  // NFC 任务写 / loop 读写
static IPAddress s_shipIp;             // 预解析缓存：flush 走 IP 直连，跳过 DNS
static bool s_shipIpOk = false;

static void ringLock() {
  if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void ringUnlock() {
  if (s_mtx) xSemaphoreGive(s_mtx);
}

void logShipDiag(uint8_t* why, uint32_t* largest8, uint32_t* cap,
                 uint32_t* pend, uint32_t* attempt) {
  if (why) *why = s_why;
  if (largest8) *largest8 = s_largest8;
  if (cap) *cap = s_cap;
  if (attempt) *attempt = s_attempt;
  if (pend) {
    ringLock();
    *pend = (uint32_t)s_len;
    ringUnlock();
  }
}

static void ringPush(const char* s, size_t n) {
  if (n == 0) return;
  if (n >= LOG_SHIP_RING_BYTES) {
    s += (n - LOG_SHIP_RING_BYTES) + 1;
    n = LOG_SHIP_RING_BYTES - 1;
  }
  if (s_len + n >= LOG_SHIP_RING_BYTES) {
    size_t drop = s_len + n - (LOG_SHIP_RING_BYTES - 1);
    if (drop >= s_len) {
      s_len = 0;
    } else {
      memmove(s_ring, s_ring + drop, s_len - drop);
      s_len -= drop;
    }
  }
  memcpy(s_ring + s_len, s, n);
  s_len += n;
}

// 发送失败把快照塞回队头（新日志已在后面）
static void ringPrepend(const char* s, size_t n) {
  if (n == 0) return;
  if (n >= LOG_SHIP_RING_BYTES) {
    s += (n - (LOG_SHIP_RING_BYTES - 1));
    n = LOG_SHIP_RING_BYTES - 1;
    s_len = 0;
  }
  if (s_len + n >= LOG_SHIP_RING_BYTES) {
    size_t drop = s_len + n - (LOG_SHIP_RING_BYTES - 1);
    if (drop >= s_len) {
      s_len = 0;
    } else {
      memmove(s_ring, s_ring + drop, s_len - drop);
      s_len -= drop;
    }
  }
  memmove(s_ring + n, s_ring, s_len);
  memcpy(s_ring, s, n);
  s_len += n;
}

void logShipBegin() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  ringLock();
  s_len = 0;
  ringUnlock();
  s_nextMs = millis() + 8000;
  s_failStreak = 0;
  s_inFlight = false;
  Serial.println("[LOGSHIP] begin url=" LOG_SHIP_URL);
}

void logShipPrintln(const String& line) {
  Serial.println(line);
  String t = line + "\n";
  ringLock();
  ringPush(t.c_str(), t.length());
  ringUnlock();
}

void logShipf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  size_t n = strlen(buf);
  if (n + 1 < sizeof(buf)) {
    buf[n] = '\n';
    buf[n + 1] = '\0';
    ringLock();
    ringPush(buf, n + 1);
    ringUnlock();
  }
}

size_t logShipPending() {
  ringLock();
  size_t n = s_len;
  ringUnlock();
  return n;
}

// 解析 LOG_SHIP_URL → host/port/path
static void parseLogUrl(String* host, uint16_t* port, String* path) {
  *host = LOG_SHIP_HOST;
  *port = 80;
  *path = "/dev/logs";
  String url = LOG_SHIP_URL;
  if (!url.startsWith("http://")) return;
  String rest = url.substring(7);
  int slash = rest.indexOf('/');
  String hp = slash >= 0 ? rest.substring(0, slash) : rest;
  *path = slash >= 0 ? rest.substring(slash) : String("/dev/logs");
  int c = hp.indexOf(':');
  if (c >= 0) {
    *host = hp.substring(0, c);
    *port = (uint16_t)atoi(hp.substring(c + 1).c_str());
  } else {
    *host = hp;
  }
}

static void buildPath(String* path) {
  if (path->indexOf("id=") < 0) {
    *path += (path->indexOf('?') >= 0 ? '&' : '?');
    *path += "id=";
    *path += deviceId();
  }
}

// 预解析日志服务器 IP：必须在网络正常、非停滞上下文调用（如 OTA 入口）。
// 之后 logShipFlushNow 全程用缓存 IP 直连，避开"黑洞期 DNS 阻塞 >5s → TWT"。
void logShipResolve() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress addr;
  esp_task_wdt_reset();
  // hostByName 内部有界（IDLE 16s + DONE 15s），但 loopTask 看门狗 5s 就咬：
  // OTA/flush 入口的这次解析一卡过 5s 就 TWT 复位（dda0 实测两次 OTA 均死于此
  // 类路径）。解析期间撤监控，结束立刻补喂。
  disableLoopWDT();
  bool ok = WiFi.hostByName(LOG_SHIP_HOST, addr);
  enableLoopWDT();
  esp_task_wdt_reset();
  if (ok) {
    s_shipIp = addr;
    s_shipIpOk = true;
    Serial.printf("[LOGSHIP] resolve %s -> %s\n", LOG_SHIP_HOST,
                  addr.toString().c_str());
  } else {
    s_shipIpOk = false;
    Serial.printf("[LOGSHIP] resolve failed host=%s\n", LOG_SHIP_HOST);
  }
}

// 同步刷出（OTA 重启前专用）：worker 可能还有在飞，重复发一遍无害（幂等追加）
// [FLUSH] 打点用于定位 TWT：崩溃时串口最后一条 FLUSH 行 = 卡住的段
void logShipFlushNow() {
  uint32_t t0 = millis();
  Serial.printf("[FLUSH] enter t=%u\n", (unsigned)t0);
  s_nextMs = 0;
  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);
  buildPath(&path);

  ringLock();
  String body;
  // 同样分片 + 自适应上限：整环 2560B 或固定 1536 在碎片堆 OOM 会 early-skip，
  // OTA 重启前一段日志全丢
  const size_t fmax = safeChunk();
  size_t fchunk = s_len > fmax ? fmax : s_len;
  if (fchunk > 0) {
    body.reserve(fchunk);
    body.concat(s_ring, fchunk);
    if (body.length() != fchunk) fchunk = 0;
  }
  ringUnlock();
  if (fchunk == 0 || WiFi.status() != WL_CONNECTED) {
    Serial.printf("[FLUSH] early skip dt=%u\n", (unsigned)(millis() - t0));
    return;
  }

  // 预解析缓存 IP 直连（见 logShipResolve）：网络黑洞期 DNS 可阻塞 >5s
  // → loopTask TWT 崩溃；无缓存且 resolve 失败则跳过本次 flush，
  // 日志留在环里，网络恢复后由 logShipService 补发
  if (!s_shipIpOk) logShipResolve();
  if (!s_shipIpOk) {
    Serial.printf("[FLUSH] no ip dt=%u\n", (unsigned)(millis() - t0));
    return;
  }
  esp_task_wdt_reset();
  WiFiClient client;
  bool conn = client.connect(s_shipIp, port, LOG_SHIP_TIMEOUT_MS);
  Serial.printf("[FLUSH] conn=%d dt=%u\n", (int)conn, (unsigned)(millis() - t0));
  if (!conn) return;
  esp_task_wdt_reset();
  String req;
  req.reserve(160 + body.length());
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nContent-Type: text/plain\r\n";
  req += "Content-Length: ";
  req += String((unsigned)body.length());
  req += "\r\nConnection: close\r\n\r\n";
  req += body;
  // WiFiClient::write 内部是 1s select × 10 轮，网络卡顿时最坏阻塞 10s
  // ＞ TWT 5s → loopTask 崩溃。自写受控发送：200ms 一片、每片喂狗，
  // 总预算 3s；发不完就放弃（日志留在环里，恢复后补发）
  {
    const char* p = req.c_str();
    size_t total = req.length();
    size_t sent = 0;
    uint32_t st0 = millis();
    int sfd = client.fd();
    while (sent < total) {
      esp_task_wdt_reset();
      if (millis() - st0 > 3000) break;
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET(sfd, &wset);
      struct timeval tv = {0, 200000};
      int r = select(sfd + 1, nullptr, &wset, nullptr, &tv);
      if (r < 0) break;
      if (r > 0 && FD_ISSET(sfd, &wset)) {
        int n = send(sfd, p + sent, total - sent, MSG_DONTWAIT);
        if (n > 0) {
          sent += (size_t)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          break;
        }
      }
    }
    Serial.printf("[FLUSH] sent %u/%u dt=%u\n", (unsigned)sent,
                  (unsigned)total, (unsigned)(millis() - t0));
    if (sent < total) {
      client.stop();
      return;
    }
  }
  uint32_t start = millis();
  String raw;
  while (client.connected() || client.available()) {
    if (millis() - start > LOG_SHIP_TIMEOUT_MS) break;
    while (client.available()) raw += (char)client.read();
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    delay(1);
    esp_task_wdt_reset();
    if (raw.length() > 512) break;
  }
  client.stop();
  int sp1 = raw.indexOf(' ');
  int sp2 = raw.indexOf(' ', sp1 + 1);
  int code = (sp1 >= 0 && sp2 >= 0) ? raw.substring(sp1 + 1, sp2).toInt() : -1;
  Serial.printf("[FLUSH] done code=%d dt=%u\n", code, (unsigned)(millis() - t0));
  if (code == 200) {
    ringLock();
    // 只摘已发出的环头一片；新日志在环尾不受影响
    if (fchunk <= s_len) {
      memmove(s_ring, s_ring + fchunk, s_len - fchunk);
      s_len -= fchunk;
    }
    ringUnlock();
    s_failStreak = 0;
  }
  s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
}

void logShipService(bool btBusy, bool wifiOk) {
  (void)btBusy;  // 射频仲裁在 http_client worker

  // 收结果：成功=快照已发走（ring 提交时已摘掉）；失败=塞回队头重试
  if (s_inFlight) {
    s_why = 1;
    int code = 0;
    if (!httpTryResult(HTTP_OWNER_LOGS, &code, nullptr)) return;
    s_inFlight = false;
    if (code == 200) {
      s_failStreak = 0;
      s_why = 9;
      // 环里还有积压 → 1s 后接着发下一片；发干净才回 30s 周期
      ringLock();
      size_t left = s_len;
      ringUnlock();
      s_nextMs = millis() + (left > 0 ? 1000UL : LOG_SHIP_INTERVAL_MS);
    } else {
      s_failStreak++;
      s_why = 10;  // 收到非200（网络失败/服务端拒）
      ringLock();
      ringPrepend(s_snap.c_str(), s_snap.length());
      ringUnlock();
      s_nextMs = millis() + (s_failStreak >= 3 ? 60000UL : LOG_SHIP_INTERVAL_MS);
    }
    s_snap = "";
    return;
  }

  if (!wifiOk) {
    s_why = 2;
    return;
  }
  const uint32_t now = millis();
  if ((int32_t)(now - s_nextMs) < 0) {
    s_why = 3;
    return;
  }

  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);

  // 快照环头 ≤自适应上限，见 safeChunk()/LOG_SHIP_CHUNK 注释的空包死循环；
  // 摘环仅在拷贝成功后执行——OOM 时内容留环里下轮再试（旧实现先清环会丢日志）
  const size_t s_chunkMax = safeChunk();
  s_largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s_cap = (uint32_t)s_chunkMax;
  String body;
  size_t chunk = 0;
  if (s_chunkMax == 0) {
    // 堆碎片到连小片都放不下：别摘环，3s 后重试（30s 会让黑窗拖太久）
    s_why = 4;
    s_nextMs = now + 3000;
    return;
  }
  ringLock();
  if (s_len > 0) {
    s_attempt++;
    chunk = s_len > s_chunkMax ? s_chunkMax : s_len;
    body.reserve(chunk);
    body.concat(s_ring, chunk);
    if (body.length() == chunk) {
      memmove(s_ring, s_ring + chunk, s_len - chunk);
      s_len -= chunk;
    } else {
      s_why = 6;  // body 拷贝 OOM → 不摘环，3s 重试
      chunk = 0;
    }
  }
  ringUnlock();

  if (chunk == 0) {
    // 环为空（正常静默）或 reserve 失败（留环重试）：都等下一轮
    if (s_why != 6) s_why = 5;
    s_nextMs = now + LOG_SHIP_INTERVAL_MS;
    return;
  }
  buildPath(&path);
  // 回填用的 s_snap 必须在提交前拷贝成功：环已在上面摘走，
  // 若 s_snap 拷贝 OOM 变空串，非 200 时 ringPrepend 会回填空气，日志照丢。
  s_snap = body;
  if (s_snap.length() != body.length()) {
    s_why = 7;
    s_snap = "";
    ringLock();
    ringPrepend(body.c_str(), body.length());
    ringUnlock();
    s_nextMs = now + 3000;
    return;
  }
  if (httpSubmitPost(HTTP_OWNER_LOGS, host, port, path, body,
                     LOG_SHIP_TIMEOUT_MS)) {
    s_why = 8;  // 已入队，等 worker 发出
    s_inFlight = true;
  } else {
    // 队列满 / HttpJob 拷贝失败：塞回，稍后重试
    s_why = 11;
    s_snap = "";
    ringLock();
    ringPrepend(body.c_str(), body.length());
    ringUnlock();
    s_nextMs = now + 3000;
  }
}
