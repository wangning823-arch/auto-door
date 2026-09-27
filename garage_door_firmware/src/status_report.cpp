#include "status_report.h"
#include "device_id.h"
#include "http_client.h"
#include "log_ship.h"
#include <WiFi.h>

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
static bool s_inFlight = false;

void statusReportSetBits(const StatusBits& b) { gBits = b; }

// 弱符号默认：未绑定则只报版本/网络
static void appendEscaped(String& j, const char* s) {
  for (; s && *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      j += '\\';
      j += c;
    } else if ((unsigned char)c >= 0x20) {
      j += c;
    }
  }
}

static String buildJson() {
  String j;
  j.reserve(800);
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
  // ===== 控制台配置回显 =====
  j += ",\"sta_ip\":\"";
  appendEscaped(j, gBits.staIp);
  j += "\",\"mac\":\"";
  appendEscaped(j, gBits.mac);
  j += "\",\"mode\":" + String(gBits.trackMode);
  j += ",\"autotrack\":" + String(gBits.autoTrack ? 1 : 0);
  j += ",\"pair\":{\"open\":" + String(gBits.pairOpen ? 1 : 0);
  j += ",\"pin\":" + String(gBits.pairHasPin ? 1 : 0) + "}";
  j += ",\"car_rssi\":" + String(gBits.carRssi);
  j += ",\"trend\":" + String(gBits.trend);
  j += ",\"ble\":{\"rssi\":" + String(gBits.bleRssi);
  j += ",\"label\":\"";
  appendEscaped(j, gBits.bleLab);
  j += "\"}";
  j += "}";
  return j;
}

void statusReportBegin() {
  s_nextMs = millis() + 5000;
  s_force = false;
  s_inFlight = false;
  Serial.println("[STATUS] begin url=" STATUS_REPORT_URL);
}

void statusReportNow() {
  s_force = true;
  s_nextMs = 0;
}

void statusReportService(bool btBusy, bool wifiOk) {
  (void)btBusy;  // 射频仲裁在 http_client worker

  if (s_inFlight) {
    int code = 0;
    if (!httpTryResult(HTTP_OWNER_STATUS, &code, nullptr)) return;
    s_inFlight = false;
    s_force = false;
    if (code == 200) {
      s_nextMs = millis() + STATUS_REPORT_INTERVAL_MS;
    } else {
      s_nextMs = millis() + 30000UL;
    }
    return;
  }

  if (!wifiOk) return;
  const uint32_t now = millis();
  if (!s_force && (int32_t)(now - s_nextMs) < 0) return;

  String url = STATUS_REPORT_URL;
  String host = "door.wzx.homes";
  uint16_t port = 80;
  String path = "/dev/status";
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
  path += (path.indexOf('?') >= 0) ? '&' : '?';
  path += "id=";
  path += deviceId();

  if (httpSubmitPost(HTTP_OWNER_STATUS, host, port, path, buildJson(),
                     STATUS_REPORT_TIMEOUT_MS)) {
    s_inFlight = true;
    s_force = false;
  } else {
    s_force = false;
    s_nextMs = now + 3000;
  }
}
