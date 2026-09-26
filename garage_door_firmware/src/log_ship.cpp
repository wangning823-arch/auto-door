#include "log_ship.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include <WiFi.h>
#include <WiFiClient.h>
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

static char s_ring[LOG_SHIP_RING_BYTES];
static size_t s_len = 0;  // 有效字节，紧凑存放
static uint32_t s_nextMs = 0;
static int s_failStreak = 0;
static bool s_inFlight = false;      // 快照已提交、结果未收
static String s_snap;                 // 在飞的请求快照（失败时塞回）
static SemaphoreHandle_t s_mtx = nullptr;  // NFC 任务写 / loop 读写

static void ringLock() {
  if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void ringUnlock() {
  if (s_mtx) xSemaphoreGive(s_mtx);
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

// 同步刷出（OTA 重启前专用）：worker 可能还有在飞，重复发一遍无害（幂等追加）
void logShipFlushNow() {
  s_nextMs = 0;
  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);
  buildPath(&path);

  ringLock();
  String body;
  body.reserve(s_len);
  body.concat(s_ring, s_len);
  ringUnlock();
  if (body.length() == 0 || WiFi.status() != WL_CONNECTED) return;

  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) return;
  WiFiClient client;
  if (!client.connect(addr, port, LOG_SHIP_TIMEOUT_MS)) return;
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
  client.print(req);
  uint32_t start = millis();
  String raw;
  while (client.connected() || client.available()) {
    if (millis() - start > LOG_SHIP_TIMEOUT_MS) break;
    while (client.available()) raw += (char)client.read();
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    delay(1);
    if (raw.length() > 512) break;
  }
  client.stop();
  int sp1 = raw.indexOf(' ');
  int sp2 = raw.indexOf(' ', sp1 + 1);
  if (sp1 >= 0 && sp2 >= 0 && raw.substring(sp1 + 1, sp2).toInt() == 200) {
    ringLock();
    s_len = 0;
    ringUnlock();
    s_failStreak = 0;
  }
  s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
}

void logShipService(bool btBusy, bool wifiOk) {
  (void)btBusy;  // 射频仲裁在 http_client worker

  // 收结果：成功=快照已发走（ring 提交时已摘掉）；失败=塞回队头重试
  if (s_inFlight) {
    int code = 0;
    if (!httpTryResult(HTTP_OWNER_LOGS, &code, nullptr)) return;
    s_inFlight = false;
    if (code == 200) {
      s_failStreak = 0;
      s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
    } else {
      s_failStreak++;
      ringLock();
      ringPrepend(s_snap.c_str(), s_snap.length());
      ringUnlock();
      s_nextMs = millis() + (s_failStreak >= 3 ? 60000UL : LOG_SHIP_INTERVAL_MS);
    }
    s_snap = "";
    return;
  }

  if (!wifiOk) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_nextMs) < 0) return;

  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);

  // 快照并摘掉（飞行期间新日志落新 ring，互不干扰）
  String body;
  ringLock();
  if (s_len > 0) {
    body.reserve(s_len);
    body.concat(s_ring, s_len);
    s_len = 0;
  }
  ringUnlock();

  if (body.length() == 0) {
    s_nextMs = now + LOG_SHIP_INTERVAL_MS;
    return;
  }
  buildPath(&path);
  if (httpSubmitPost(HTTP_OWNER_LOGS, host, port, path, body,
                     LOG_SHIP_TIMEOUT_MS)) {
    s_snap = body;
    s_inFlight = true;
  } else {
    // 队列满：塞回，稍后重试
    ringLock();
    ringPrepend(body.c_str(), body.length());
    ringUnlock();
    s_nextMs = now + 3000;
  }
}
