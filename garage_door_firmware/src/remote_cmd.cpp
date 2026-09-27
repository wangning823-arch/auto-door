#include "remote_cmd.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include "log_ship.h"

#include <WiFi.h>

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
static uint32_t s_lastPollMs = 0;
static int s_failStreak = 0;
static int s_okStreak = 0;
static bool s_inFlight = false;  // 已提交、结果未收

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
  s_inFlight = false;
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

// 手写 HTTP 已移至 http_client（独立任务异步执行，loop 不阻塞）

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
  (void)btBusy;  // 射频仲裁移到 http_client：worker 发送前自己等蓝牙空隙
  const uint32_t now = millis();

  // 在飞：只收结果，不发新请求（收结果零成本；wifi 断了也要收，防 owner 卡死）
  if (s_inFlight) {
    int code = 0;
    String body;
    if (!httpTryResult(HTTP_OWNER_POLL, &code, &body)) return;
    s_inFlight = false;
    s_lastPollMs = millis();
    // 失败退避：连挂时不要每 3s 再撞网络
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
    // 带参指令（mac AA:BB:... / wifista ssid pass …）需要更大缓冲
    char cmd[128] = {0};
    if (!parseCmd(body, cmd, sizeof(cmd))) {
      if (s_okStreak == 1 || (s_okStreak % 20) == 0) {
        Serial.printf("[REMOTE] poll ok x%d (idle)\n", s_okStreak);
      }
      return;
    }

    logShipf("[REMOTE] cmd=%s", cmd);
    if (s_fn) s_fn(cmd);
    return;
  }

  if (!s_enabled || !wifiOk) return;
  if (!millisReached(now, s_nextMs)) return;
  if (s_lastPollMs != 0 && (now - s_lastPollMs) < REMOTE_POLL_INTERVAL_MS) {
    return;
  }

  if (s_memTrim) s_memTrim();

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

  // 提交即返回；结果下一轮 loop 来收（loop 永不等网络）
  if (httpSubmitGet(HTTP_OWNER_POLL, host, port, path,
                    REMOTE_POLL_TIMEOUT_MS)) {
    s_inFlight = true;
  } else {
    s_nextMs = now + 1000;  // 队列忙，1s 后再试
  }
}
