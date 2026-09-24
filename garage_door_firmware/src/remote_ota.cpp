#include "remote_ota.h"
#include "config.h"
#include "device_id.h"
#include "log_ship.h"
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>

#ifndef OTA_VERSION_URL
#define OTA_VERSION_URL "http://door.wzx.homes/ota/version"
#endif
#ifndef OTA_BIN_URL
#define OTA_BIN_URL "http://door.wzx.homes/ota/firmware.bin"
#endif
#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)  // 兜底；开发靠 poll update 令
#endif
#ifndef OTA_HTTP_TIMEOUT_MS
#define OTA_HTTP_TIMEOUT_MS 8000
#endif

static ConfigStore* s_cfg = nullptr;
static OtaBusyFn s_busyFn = nullptr;
static uint32_t s_nextMs = 0;
static bool s_force = false;
static bool s_active = false;
static bool s_done = false;
static char s_lastMsg[64] = "idle";

static void setMsg(const char* m) {
  strncpy(s_lastMsg, m, sizeof(s_lastMsg) - 1);
  s_lastMsg[sizeof(s_lastMsg) - 1] = 0;
}

void remoteOtaBegin(ConfigStore* cfg) {
  s_cfg = cfg;
  // 上电 90s 后才查，避开 DHCP/BT 初始化
  s_nextMs = millis() + 90000UL;
  s_force = false;
  s_done = false;
  logShipf("[OTA] remote check every %umin url=%s",
           (unsigned)(OTA_CHECK_INTERVAL_MS / 60000UL), OTA_VERSION_URL);
}

void remoteOtaSetBusyHook(OtaBusyFn fn) { s_busyFn = fn; }
void remoteOtaCheckNow() {
  s_force = true;
  s_nextMs = 0;
}
bool remoteOtaActive() { return s_active; }
const char* remoteOtaLastMsg() { return s_lastMsg; }

static bool httpGetStream(const String& host, uint16_t port, const String& path,
                          WiFiClient* client, long* contentLen) {
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) return false;
  if (!client->connect(addr, port, OTA_HTTP_TIMEOUT_MS)) return false;
  String req;
  req.reserve(128);
  req += "GET ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nConnection: close\r\n\r\n";
  if (client->print(req) != (int)req.length()) {
    client->stop();
    return false;
  }
  // 读响应头
  uint32_t start = millis();
  String head;
  while (client->connected() || client->available()) {
    if (millis() - start > OTA_HTTP_TIMEOUT_MS) break;
    while (client->available()) {
      char c = (char)client->read();
      head += c;
      if (head.indexOf("\r\n\r\n") >= 0) break;
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
  // 头后残留 body 仍在 socket 里，交给调用方继续 read
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

static void doOta() {
  s_force = false;
  if (s_active || s_done) return;

  String host = "door.wzx.homes";
  uint16_t port = 80;
  String vpath = "/ota/version";
  {
    String url = OTA_VERSION_URL;
    if (url.startsWith("http://")) {
      String rest = url.substring(7);
      int slash = rest.indexOf('/');
      String hp = slash >= 0 ? rest.substring(0, slash) : rest;
      vpath = slash >= 0 ? rest.substring(slash) : String("/ota/version");
      int c = hp.indexOf(':');
      if (c >= 0) {
        host = hp.substring(0, c);
        port = (uint16_t)atoi(hp.substring(c + 1).c_str());
      } else {
        host = hp;
      }
    }
  }
  vpath = withId(vpath);

  WiFiClient client;
  long clen = -1;
  if (!httpGetStream(host, port, vpath, &client, &clen)) {
    setMsg("version fetch fail");
    logShipf("[OTA] version fetch fail id=%s", deviceId().c_str());
    return;
  }
  String body;
  body.reserve(256);
  uint32_t start = millis();
  while (client.connected() || client.available()) {
    if (millis() - start > 2000) break;
    while (client.available()) body += (char)client.read();
    delay(1);
    if (body.length() > 512) break;
  }
  client.stop();

  String remoteVer;
  if (!parseJsonStr(body, "version", &remoteVer)) {
    setMsg("no version");
    logShipf("[OTA] version.json missing version");
    return;
  }
  if (remoteVer == FW_VERSION) {
    setMsg("up to date");
    logShipf("[OTA] up to date %s", FW_VERSION);
    return;
  }
  logShipf("[OTA] new %s -> %s id=%s", FW_VERSION, remoteVer.c_str(),
           deviceId().c_str());

  s_active = true;
  if (s_busyFn) s_busyFn(true);

  long fsize = -1;
  String bpath = withId("/ota/firmware.bin");
  if (!httpGetStream(host, port, bpath, &client, &fsize) || fsize <= 0) {
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    setMsg("bin fetch fail");
    logShipf("[OTA] bin fetch fail id=%s", deviceId().c_str());
    return;
  }
  logShipf("[OTA] bin ok size=%ld maxblk=%u", fsize,
           (unsigned)ESP.getMaxAllocHeap());

  if (!Update.begin(fsize > 0 ? (size_t)fsize : UPDATE_SIZE_UNKNOWN)) {
    client.stop();
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    setMsg("update begin fail");
    logShipf("[OTA] Update.begin fail err=%s maxblk=%u", Update.errorString(),
             (unsigned)ESP.getMaxAllocHeap());
    return;
  }

  uint8_t buf[1024];
  size_t written = 0;
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
      client.stop();
      s_active = false;
      if (s_busyFn) s_busyFn(false);
      setMsg("write fail");
      logShipf("[OTA] write fail at %u err=%s", (unsigned)written,
               Update.errorString());
      return;
    }
    written += (size_t)r;
    start = millis();
    // 1.9MB 边下边写会堵死 loop → 看门狗复位；必须让出
    yield();
    if ((written & 0x7FFF) == 0) {
      logShipf("[OTA] write %u/%ld", (unsigned)written, fsize);
    }
  }
  client.stop();

  if (!Update.end(true)) {
    s_active = false;
    if (s_busyFn) s_busyFn(false);
    setMsg("end fail");
    logShipf("[OTA] Update.end fail err=%s", Update.errorString());
    return;
  }
  setMsg("rebooting");
  logShipf("[OTA] OK bytes=%u id=%s -> reboot", (unsigned)written,
           deviceId().c_str());
  logShipFlushNow();
  delay(300);
  ESP.restart();
}

void remoteOtaService(bool btBusy, bool wifiOk) {
  if (s_active || s_done || !wifiOk) return;
  const uint32_t now = millis();
  if (!s_force && (int32_t)(now - s_nextMs) < 0) return;
  // 蓝牙忙不写 flash；但 update 令/到点检查等太久则插队（否则 Inquiry 几乎常亮会饿死 OTA）
  static uint32_t s_waitMs = 0;
  if (btBusy && !s_force) {
    if (!s_waitMs) s_waitMs = now;
    if ((now - s_waitMs) < 15000UL) return;
  }
  s_waitMs = 0;
  s_nextMs = millis() + OTA_CHECK_INTERVAL_MS;
  doOta();
}
