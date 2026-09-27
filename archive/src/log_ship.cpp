#include "log_ship.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClient.h>

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
static bool s_force = false;
static int s_failStreak = 0;

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

void logShipBegin() {
  s_len = 0;
  s_nextMs = millis() + 8000;
  s_failStreak = 0;
  Serial.println("[LOGSHIP] begin url=" LOG_SHIP_URL);
}

void logShipPrintln(const String& line) {
  Serial.println(line);
  String t = line + "\n";
  ringPush(t.c_str(), t.length());
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
    ringPush(buf, n + 1);
  }
}

size_t logShipPending() { return s_len; }
void logShipFlushNow() {
  s_force = true;
  s_nextMs = 0;
}

static bool httpPostLogs(const String& host, uint16_t port, const String& path,
                         const String& body) {
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) return false;
  WiFiClient client;
  if (!client.connect(addr, port, LOG_SHIP_TIMEOUT_MS)) return false;

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
  if (client.print(req) != (int)req.length()) {
    client.stop();
    return false;
  }
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
  if (sp1 < 0 || sp2 < 0) return false;
  return raw.substring(sp1 + 1, sp2).toInt() == 200;
}

void logShipService(bool btBusy, bool wifiOk) {
  if (!wifiOk) return;
  const uint32_t now = millis();
  if (!s_force && (int32_t)(now - s_nextMs) < 0) return;
  if (s_len == 0) {
    s_force = false;
    s_nextMs = now + LOG_SHIP_INTERVAL_MS;
    return;
  }
  // 蓝牙忙默认不发；积压 >1.2KB 或强制时抢一次（短请求）
  if (btBusy && !s_force && s_len < 1200) return;

  // 解析 URL
  String host = LOG_SHIP_HOST;
  uint16_t port = 80;
  String path = "/dev/logs";
  String url = LOG_SHIP_URL;
  if (url.startsWith("http://")) {
    String rest = url.substring(7);
    int slash = rest.indexOf('/');
    String hp = slash >= 0 ? rest.substring(0, slash) : rest;
    path = slash >= 0 ? rest.substring(slash) : String("/dev/logs");
    int c = hp.indexOf(':');
    if (c >= 0) {
      host = hp.substring(0, c);
      port = (uint16_t)atoi(hp.substring(c + 1).c_str());
    } else {
      host = hp;
    }
  }

  String body;
  body.reserve(s_len);
  body.concat(s_ring, s_len);
  const bool ok = httpPostLogs(host, port, path, body);
  s_force = false;
  if (ok) {
    s_len = 0;
    s_failStreak = 0;
    s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
  } else {
    s_failStreak++;
    // 失败退避，避免每轮撞网络
    s_nextMs = millis() + (s_failStreak >= 3 ? 60000UL : LOG_SHIP_INTERVAL_MS);
  }
}
