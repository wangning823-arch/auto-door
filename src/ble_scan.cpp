#include "ble_scan.h"
#include "config.h"
#include <WiFi.h>

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BLUEDROID_ENABLED)
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static std::vector<BleAdvHit>* gSink = nullptr;
static bool gBleInited = false;

static String hexDump(const uint8_t* d, size_t n, size_t maxN = 24) {
  String s;
  size_t m = n < maxN ? n : maxN;
  for (size_t i = 0; i < m; i++) {
    if (d[i] < 16) s += '0';
    s += String(d[i], HEX);
    if (i + 1 < m) s += ' ';
  }
  if (n > maxN) s += "...";
  s.toUpperCase();
  return s;
}

class AdvCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!gSink) return;
    BleAdvHit h;
    h.addr = String(dev.getAddress().toString().c_str());
    h.addr.toUpperCase();
    h.rssi = dev.getRSSI();
    if (dev.haveName()) h.name = String(dev.getName().c_str());
    if (dev.haveServiceUUID()) {
      h.services = String(dev.getServiceUUID().toString().c_str());
    }
    if (dev.haveManufacturerData()) {
      std::string md = dev.getManufacturerData();
      h.mfg = hexDump((const uint8_t*)md.data(), md.size());
    }
    for (auto& x : *gSink) {
      if (x.addr == h.addr) {
        if (h.rssi > x.rssi) x.rssi = h.rssi;
        if (h.name.length() && !x.name.length()) x.name = h.name;
        if (h.services.length() && !x.services.length()) x.services = h.services;
        if (h.mfg.length() && !x.mfg.length()) x.mfg = h.mfg;
        return;
      }
    }
    gSink->push_back(h);
  }
};

static bool isXiaomiCarish(const BleAdvHit& h) {
  String n = h.name;
  n.toUpperCase();
  if (n.startsWith("MICAR")) return true;
  String s = h.services;
  s.toLowerCase();
  if (s.indexOf("fcd1") >= 0) return true;
  if (h.addr.startsWith("58:C4:1E")) return true;
  return false;
}

bool BleScanTool::matchHits(const BleAdvHit& h) const {
  if (filter_.length() == 0) return false;
  String f = filter_;
  f.toUpperCase();
  String n = h.name;
  n.toUpperCase();
  String a = h.addr;
  a.toUpperCase();
  if (f.indexOf(':') > 0) {
    return a.indexOf(f) >= 0;
  }
  return n.indexOf(f) >= 0;
}

std::vector<BleAdvHit> BleScanTool::interestingHits() const {
  std::vector<BleAdvHit> out;
  for (const auto& h : hits_) {
    if (isXiaomiCarish(h) || matchHits(h)) out.push_back(h);
  }
  for (size_t i = 0; i < out.size(); i++) {
    for (size_t j = i + 1; j < out.size(); j++) {
      if (out[j].rssi > out[i].rssi) {
        BleAdvHit t = out[i];
        out[i] = out[j];
        out[j] = t;
      }
    }
  }
  return out;
}

void BleScanTool::runScan(uint32_t durationMs) {
  if (busy_) {
    Serial.println("[BLE] scan already running");
    return;
  }
  // 距上次扫太近容易 err 259；至少隔 2.5s
  if (millis() - lastScanEndMs_ < 2500) {
    return;
  }
  busy_ = true;
  hits_.clear();
  // 不要清 matchRssi_：失败时保留上次成功值，避免网页显示 -

  Serial.printf("[BLE] scan %ums filter=\"%s\"\n", durationMs,
                filter_.c_str());

  if (!gBleInited) {
    BLEDevice::init("GarageDoorBLE");
    gBleInited = true;
  }

  BLEScan* scan = BLEDevice::getScan();
  AdvCb cb;
  gSink = &hits_;
  scan->setAdvertisedDeviceCallbacks(&cb, true);
  scan->setActiveScan(true);
  scan->setInterval(120);
  scan->setWindow(90);
  // 连续扫描时 duration 用秒；1s 太短，跟踪用 >=1.5s
  uint32_t sec = (durationMs + 999) / 1000;
  if (sec < 2) sec = 2;
  if (sec > 15) sec = 15;

  scan->start(sec, false);
  gSink = nullptr;
  lastScanEndMs_ = millis();

  int best = -127;
  String label;
  for (const auto& h : hits_) {
    if (matchHits(h) && h.rssi > best) {
      best = h.rssi;
      label = h.name.length() ? h.name : h.addr;
    }
  }
  lostCar_ = false;
  if (best > -127) {
    matchRssi_ = best;
    matchLabel_ = label;
    lastMatchMs_ = millis();
    missStreak_ = 0;
  } else {
    missStreak_ = (missStreak_ < 200) ? (uint8_t)(missStreak_ + 1) : 200;
    if (missStreak_ >= BLE_MISS_FOR_LOST) {
      lostCar_ = true;
    }
  }

  Serial.printf("[BLE] hits=%u match_rssi=%d label=%s miss=%u%s\n",
                (unsigned)hits_.size(), matchRssi_, matchLabel_.c_str(),
                missStreak_, lostCar_ ? " LOST" : "");
  auto list = interestingHits();
  size_t show = list.size() < 8 ? list.size() : 8;
  for (size_t i = 0; i < show; i++) {
    const BleAdvHit& h = list[i];
    Serial.printf("[BLE] %s rssi=%d name=\"%s\"%s\n", h.addr.c_str(), h.rssi,
                  h.name.c_str(), matchHits(h) ? " <-MATCH" : "");
  }

  scan->clearResults();
  busy_ = false;
}

void BleScanTool::trackPoll(uint32_t intervalMs, uint32_t scanMs) {
  if (!trackOn_ || busy_) return;
  if (millis() < nextTrackMs_) return;
  nextTrackMs_ = millis() + intervalMs;
  // 跟踪扫长一点，提高命中率
  if (scanMs < 2000) scanMs = 2000;
  runScan(scanMs);
}

#else
void BleScanTool::runScan(uint32_t) {
  Serial.println("[BLE] BLE not enabled in this build");
}
void BleScanTool::trackPoll(uint32_t, uint32_t) {}
std::vector<BleAdvHit> BleScanTool::interestingHits() const { return {}; }
bool BleScanTool::matchHits(const BleAdvHit&) const { return false; }
#endif
