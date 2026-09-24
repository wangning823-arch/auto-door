#include "remote_cmd.h"
#include "config.h"
#include "device_id.h"
#include "log_ship.h"

#include <WiFi.h>
#include <WiFiClient.h>

// 仅明文 HTTP：WiFiClientSecure / mbedTLS 已删除（TLS 堆起不来，留着白占 flash）

#ifndef REMOTE_CMD_ENABLE_DEFAULT
#define REMOTE_CMD_ENABLE_DEFAULT 0
#endif
#ifndef REMOTE_POLL_URL
#define REMOTE_POLL_URL "http://door.wzx.homes/dev/poll"
#endif
#ifndef REMOTE_POLL_INTERVAL_MS
#define REMOTE_POLL_INTERVAL_MS 3000
#endif
#ifndef REMOTE_POLL_TIMEOUT_MS
#define REMOTE_POLL_TIMEOUT_MS 2500
#endif
#ifndef REMOTE_BT_GAP_MIN_MS
#define REMOTE_BT_GAP_MIN_MS 100
#endif
#ifndef REMOTE_HTTP_HOST
#define REMOTE_HTTP_HOST "door.wzx.homes"
#endif
#ifndef REMOTE_HTTP_PORT
#define REMOTE_HTTP_PORT 80
#endif
#ifndef REMOTE_HTTP_PATH
#define REMOTE_HTTP_PATH "/dev/poll"
#endif
// 连续失败后拉长间隔（秒级退避）
#ifndef REMOTE_FAIL_BACKOFF_MS
#define REMOTE_FAIL_BACKOFF_MS 20000
#endif

static RemoteCmdFn s_fn = nullptr;
static RemoteMemTrimFn s_memTrim = nullptr;
static ConfigStore* s_cfg = nullptr;
static bool s_enabled = REMOTE_CMD_ENABLE_DEFAULT;
static uint32_t s_nextMs = 0;
static uint32_t s_btBusyUntilMs = 0;
static uint32_t s_lastPollMs = 0;
static int s_failStreak = 0;
static int s_okStreak = 0;

void remoteCmdSetHandler(RemoteCmdFn fn) { s_fn = fn; }
void remoteCmdSetMemTrim(RemoteMemTrimFn fn) { s_memTrim = fn; }
bool remoteCmdEnabled() { return s_enabled; }

void remoteCmdSetEnabled(bool on) {
  s_enabled = on;
  if (s_cfg) s_cfg->saveRemote(on);
  Serial.printf("[REMOTE] %s (url=%s)%s\n", on ? "ON" : "OFF", REMOTE_POLL_URL,
                s_cfg ? " [NVS]" : "");
}

void remoteCmdBegin(ConfigStore* cfg) {
  s_cfg = cfg;
  s_enabled = cfg ? cfg->loadRemote(REMOTE_CMD_ENABLE_DEFAULT != 0)
                  : (REMOTE_CMD_ENABLE_DEFAULT != 0);
  s_nextMs = millis() + 1500;
  s_lastPollMs = 0;
  s_failStreak = 0;
  s_okStreak = 0;
  Serial.printf(
      "[REMOTE] begin enable=%d interval=%ums timeout=%ums http=%s:%u%s\n",
      s_enabled ? 1 : 0, (unsigned)REMOTE_POLL_INTERVAL_MS,
      (unsigned)REMOTE_POLL_TIMEOUT_MS, REMOTE_HTTP_HOST,
      (unsigned)REMOTE_HTTP_PORT, REMOTE_HTTP_PATH);
}

static bool parseCmd(const String& body, char* out, size_t outLen) {
  // 要求 "cmd" 后是带引号的字符串；null/数字不要当指令
  int i = body.indexOf("\"cmd\"");
  if (i < 0) return false;
  int colon = body.indexOf(':', i + 5);
  if (colon < 0) return false;
  int p = colon + 1;
  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  if (p >= (int)body.length() || body[p] != '"') return false;  // null 等
  int q1 = p;
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0 || q2 - q1 >= (int)outLen) return false;
  body.substring(q1 + 1, q2).toCharArray(out, outLen);
  if (out[0] == '\0') return false;
  return true;
}

// 手写 HTTP GET：绕开 ESP32 HTTPClient 被 nginx 判 400 的问题
static int httpGetRaw(Client& client, const String& host, const String& path,
                      String* bodyOut) {
  String req;
  req.reserve(160);
  req += "GET ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nAccept: application/json\r\n";
  req += "Connection: close\r\n\r\n";
  if (client.print(req) != (int)req.length()) {
    client.stop();
    Serial.println("[REMOTE] send fail");
    return -2;
  }

  uint32_t start = millis();
  String raw;
  raw.reserve(512);
  while (client.connected() || client.available()) {
    if (millis() - start > REMOTE_POLL_TIMEOUT_MS) {
      client.stop();
      Serial.println("[REMOTE] read timeout");
      return -3;
    }
    while (client.available()) {
      raw += (char)client.read();
    }
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    delay(1);
    if (raw.length() > 4096) break;
  }
  uint32_t tail = millis();
  while (client.available() && millis() - tail < 500) {
    raw += (char)client.read();
  }
  client.stop();

  int hdrEnd = raw.indexOf("\r\n\r\n");
  if (hdrEnd < 0) {
    Serial.printf("[REMOTE] no http header len=%u\n", (unsigned)raw.length());
    return -4;
  }
  String head = raw.substring(0, hdrEnd);
  String body = raw.substring(hdrEnd + 4);
  if (bodyOut) *bodyOut = body;

  int sp1 = head.indexOf(' ');
  int sp2 = head.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return -5;
  return head.substring(sp1 + 1, sp2).toInt();
}

static int httpGetPlain(const String& host, uint16_t port, const String& path,
                        String* bodyOut) {
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) {
    Serial.println("[REMOTE] dns fail");
    return -11;
  }
  WiFiClient client;
  if (!client.connect(addr, port, REMOTE_POLL_TIMEOUT_MS)) {
    Serial.println("[REMOTE] http connect fail");
    return -1;
  }
  return httpGetRaw(client, host, path, bodyOut);
}

// 仅支持 http://（https 已随 TLS 一并删除）
static bool parseUrl(const String& url, String* host, uint16_t* port,
                     String* path) {
  String u = url;
  *port = 80;
  if (u.startsWith("http://")) {
    u = u.substring(7);
  } else {
    return false;  // https:// 或裸 host 一律拒绝
  }
  int slash = u.indexOf('/');
  String hp = slash >= 0 ? u.substring(0, slash) : u;
  *path = slash >= 0 ? u.substring(slash) : String("/");
  int colon = hp.indexOf(':');
  if (colon >= 0) {
    *host = hp.substring(0, colon);
    *port = (uint16_t)atoi(hp.substring(colon + 1).c_str());
  } else {
    *host = hp;
  }
  return host->length() > 0;
}

void remoteCmdService(bool btBusy, bool wifiOk) {
  const uint32_t now = millis();
  if (!s_enabled || !wifiOk) return;

  // 蓝牙忙默认让路；超过 6s 没轮询成功则插队，避免 8s TTL 过期
  const bool stale = (s_lastPollMs == 0) || ((now - s_lastPollMs) > 6000);
  if (btBusy && !stale) {
    s_btBusyUntilMs = now;
    return;
  }
  if (!btBusy && (now - s_btBusyUntilMs) < REMOTE_BT_GAP_MIN_MS) return;

  if (!millisReached(now, s_nextMs)) return;
  if (s_lastPollMs != 0 && (now - s_lastPollMs) < REMOTE_POLL_INTERVAL_MS) {
    return;
  }

  if (s_memTrim) s_memTrim();

  String body;
  int code = -1;
  String host;
  uint16_t port = 80;
  String path = REMOTE_HTTP_PATH;
  if (!parseUrl(REMOTE_POLL_URL, &host, &port, &path)) {
    host = REMOTE_HTTP_HOST;
    port = REMOTE_HTTP_PORT;
    path = REMOTE_HTTP_PATH;
  }
  // 带上设备 id + 固件版本：多板不抢指令；落后时直接回 {"cmd":"update"}
  {
    String q = path;
    q += (path.indexOf('?') >= 0) ? '&' : '?';
    q += "id=";
    q += deviceId();
    q += "&fw=";
    q += FW_VERSION;
    path = q;
  }

  code = httpGetPlain(host, port, path, &body);
  s_lastPollMs = millis();
  // 失败退避：连挂时不要每 3s 再撞网络（会饿死 Web handleClient / NFC）
  if (code == 200) {
    s_nextMs = millis() + REMOTE_POLL_INTERVAL_MS;
  } else if (s_failStreak >= 2) {
    s_nextMs = millis() + REMOTE_FAIL_BACKOFF_MS;
  } else {
    s_nextMs = millis() + REMOTE_POLL_INTERVAL_MS;
  }

  if (code != 200) {
    s_failStreak++;
    if (s_failStreak <= 5 || (s_failStreak % 10) == 0) {
      Serial.printf("[REMOTE] HTTP %d streak=%d heap=%u next=+%ums\n", code,
                    s_failStreak, (unsigned)ESP.getFreeHeap(),
                    (unsigned)(s_nextMs - millis()));
    }
    return;
  }

  s_failStreak = 0;
  s_okStreak++;
  char cmd[16] = {0};
  if (!parseCmd(body, cmd, sizeof(cmd))) {
    if (s_okStreak == 1 || (s_okStreak % 20) == 0) {
      Serial.printf("[REMOTE] poll ok x%d (idle)\n", s_okStreak);
    }
    return;
  }

  logShipf("[REMOTE] cmd=%s", cmd);
  if (s_fn) s_fn(cmd);
}
