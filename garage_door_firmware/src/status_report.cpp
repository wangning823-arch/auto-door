#include "status_report.h"
#include "device_id.h"
#include "log_ship.h"
#include <WiFi.h>
#include <WiFiClient.h>

#ifndef STATUS_REPORT_URL
#define STATUS_REPORT_URL "http://door.wzx.homes/dev/status"
#endif
#ifndef STATUS_REPORT_INTERVAL_MS
#define STATUS_REPORT_INTERVAL_MS 20000UL
#endif
#ifndef STATUS_REPORT_TIMEOUT_MS
#define STATUS_REPORT_TIMEOUT_MS 4000
#endif

static StatusBits gBits;
static uint32_t s_nextMs = 0;
static bool s_force = false;

void statusReportSetBits(const StatusBits& b) { gBits = b; }

// 弱符号默认：未绑定则只报版本/网络
static String buildJson() {
  String j;
  j.reserve(400);
  j += "{\"id\":\"" + deviceId() + "\"";
  j += ",\"fw\":\"" + String(FW_VERSION) + "\"";
  j += ",\"role\":\"" + String(gBits.role) + "\"";
  j += ",\"uptime_ms\":" + String((unsigned long)gBits.uptimeMs);
  j += ",\"heap\":" + String((unsigned)gBits.heap);
  j += ",\"maxblk\":" + String((unsigned)gBits.maxblk);
  j += ",\"sta\":" + String(gBits.sta ? 1 : 0);
  j += ",\"ap\":" + String(gBits.ap ? 1 : 0);
  j += ",\"web\":" + String(gBits.webUp ? 1 : 0);
  j += ",\"rssi\":" + String(gBits.rssi);
  j += ",\"door\":" + String(gBits.door);
  j += ",\"remote\":" + String(gBits.remoteOn ? 1 : 0);
  j += ",\"nfc\":{\"ok\":" + String(gBits.nfcOk ? 1 : 0);
  j += ",\"deferred\":" + String(gBits.nfcDeferred ? 1 : 0);
  j += ",\"absent\":" + String(gBits.nfcAbsent ? 1 : 0);
  j += ",\"listen\":" + String(gBits.nfcListen ? 1 : 0) + "}";
  j += ",\"rf\":{\"open\":" + String(gBits.rfOpen ? 1 : 0);
  j += ",\"close\":" + String(gBits.rfClose ? 1 : 0);
  j += ",\"tx_busy\":" + String(gBits.rfTxBusy ? 1 : 0) + "}";
  j += "}";
  return j;
}

static bool httpPostStatus(const String& host, uint16_t port, const String& path,
                           const String& body) {
  IPAddress addr;
  if (!WiFi.hostByName(host.c_str(), addr)) return false;
  WiFiClient client;
  if (!client.connect(addr, port, STATUS_REPORT_TIMEOUT_MS)) return false;
  String req;
  req.reserve(160 + body.length());
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nContent-Type: application/json\r\n";
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
    if (millis() - start > STATUS_REPORT_TIMEOUT_MS) break;
    while (client.available()) raw += (char)client.read();
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    delay(1);
    if (raw.length() > 256) break;
  }
  client.stop();
  int sp1 = raw.indexOf(' ');
  int sp2 = raw.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return false;
  return raw.substring(sp1 + 1, sp2).toInt() == 200;
}

void statusReportBegin() {
  s_nextMs = millis() + 5000;
  s_force = false;
  Serial.println("[STATUS] begin url=" STATUS_REPORT_URL);
}

void statusReportNow() {
  s_force = true;
  s_nextMs = 0;
}

void statusReportService(bool btBusy, bool wifiOk) {
  if (!wifiOk) return;
  const uint32_t now = millis();
  if (!s_force && (int32_t)(now - s_nextMs) < 0) return;
  if (btBusy && !s_force) {
    s_nextMs = now + 3000;
    return;
  }

  String host = "door.wzx.homes";
  uint16_t port = 80;
  String path = "/dev/status";
  String url = STATUS_REPORT_URL;
  if (url.startsWith("http://")) {
    String rest = url.substring(7);
    int slash = rest.indexOf('/');
    String hp = slash >= 0 ? rest.substring(0, slash) : rest;
    path = slash >= 0 ? rest.substring(slash) : String("/dev/status");
    int c = hp.indexOf(':');
    if (c >= 0) {
      host = hp.substring(0, c);
      port = (uint16_t)atoi(hp.substring(c + 1).c_str());
    } else {
      host = hp;
    }
  }
  path += (path.indexOf('?') >= 0 ? '&' : '?');
  path += "id=";
  path += deviceId();

  const bool ok = httpPostStatus(host, port, path, buildJson());
  s_force = false;
  if (ok) {
    s_nextMs = millis() + STATUS_REPORT_INTERVAL_MS;
  } else {
    s_nextMs = millis() + 30000UL;
  }
}
