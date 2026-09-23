#include "remote_cmd.h"
#include "config.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#ifndef REMOTE_CMD_ENABLE_DEFAULT
#define REMOTE_CMD_ENABLE_DEFAULT 0
#endif
#ifndef REMOTE_POLL_URL
#define REMOTE_POLL_URL "https://door.wzx.homes/dev/poll"
#endif
#ifndef REMOTE_HTTP_INSECURE
#define REMOTE_HTTP_INSECURE 1
#endif
#ifndef REMOTE_POLL_INTERVAL_MS
#define REMOTE_POLL_INTERVAL_MS 3000
#endif
// 连接/读超时（毫秒）。注意：WiFiClientSecure::setTimeout 参数是秒！
// 必须走 connect(..., timeout_ms) 或 setHandshakeTimeout(秒)，不能 setTimeout(ms)。
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
#define REMOTE_HTTP_PORT 443
#endif
#ifndef REMOTE_HTTP_PATH
#define REMOTE_HTTP_PATH "/dev/poll"
#endif
// 连续失败后拉长间隔（秒级退避），避免每 3s 打一次 TLS 拖死 Web/NFC
#ifndef REMOTE_FAIL_BACKOFF_MS
#define REMOTE_FAIL_BACKOFF_MS 20000
#endif
// mbedTLS 默认 16KB×2 缓冲：最大空闲块不足时必失败，直接跳过本轮
#ifndef REMOTE_MIN_MAX_ALLOC
#define REMOTE_MIN_MAX_ALLOC 42000
#endif

static RemoteCmdFn s_fn = nullptr;
static ConfigStore* s_cfg = nullptr;
static bool s_enabled = REMOTE_CMD_ENABLE_DEFAULT;
static uint32_t s_nextMs = 0;
static uint32_t s_btBusyUntilMs = 0;
static uint32_t s_lastPollMs = 0;
static int s_failStreak = 0;
static int s_okStreak = 0;

void remoteCmdSetHandler(RemoteCmdFn fn) { s_fn = fn; }
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
      "[REMOTE] begin enable=%d interval=%ums timeout=%ums https=%s:%u%s\n",
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

// 手写 HTTPS GET：绕开 ESP32 HTTPClient 被 nginx 判 400 的问题
static int httpsGet(const String& host, uint16_t port, const String& path,
                    String* bodyOut) {
  // 堆不够再连 TLS 只会 -32512，且浪费数秒
  const uint32_t maxblk = ESP.getMaxAllocHeap();
  if (maxblk < REMOTE_MIN_MAX_ALLOC) {
    Serial.printf("[REMOTE] skip tls heap maxblk=%u < %u\n",
                  (unsigned)maxblk, (unsigned)REMOTE_MIN_MAX_ALLOC);
    return -10;
  }

  // 先 DNS（hostByName 有内部超时），失败不进 TLS
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) {
    Serial.println("[REMOTE] dns fail");
    return -11;
  }

  WiFiClientSecure client;
#if REMOTE_HTTP_INSECURE
  client.setInsecure();
#endif
  // setHandshakeTimeout 参数=秒 → 内部×1000；默认库是 120s，会挂死 loop
  client.setHandshakeTimeout(4);
  // connect(ip, port, timeout_ms)：timeout 直接给 start_ssl_client（毫秒）
  // 禁止 client.setTimeout(ms)：那是秒×1000，5000 会变成 5000 秒
  if (!client.connect(addr, port, REMOTE_POLL_TIMEOUT_MS)) {
    Serial.printf("[REMOTE] tls/connect fail heap=%u maxblk=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return -1;
  }

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

  String body;
  const int code =
      httpsGet(REMOTE_HTTP_HOST, REMOTE_HTTP_PORT, REMOTE_HTTP_PATH, &body);
  s_lastPollMs = millis();
  // 失败退避：连挂时不要每 3s 再撞 TLS（会饿死 Web handleClient / NFC）
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
      Serial.printf("[REMOTE] HTTP %d streak=%d maxblk=%u next=+%ums\n", code,
                    s_failStreak, (unsigned)ESP.getMaxAllocHeap(),
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

  Serial.printf("[REMOTE] cmd=%s\n", cmd);
  if (s_fn) s_fn(cmd);
}