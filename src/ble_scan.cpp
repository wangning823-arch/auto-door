#include "ble_scan.h"
#include "ble_bond.h"
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
    if (i < m - 1) s += ' ';
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

// 仅配对 IRK 命中（名称/MAC 特征通道已移除）
bool BleScanTool::matchHits(const BleAdvHit& h) const {
  return gBleBond.matchesAddr(h.addr);
}

std::vector<BleAdvHit> BleScanTool::matchOnlyHits() const {
  std::vector<BleAdvHit> out;
  for (const auto& h : hits_) {
    if (matchHits(h)) out.push_back(h);
  }
  return out;
}

void BleScanTool::runScan(uint32_t durationMs) {
  if (busy_) {
    Serial.println("[BLE] scan already running");
    return;
  }
  if (millis() - lastScanEndMs_ < 2500) {
    return;
  }
  busy_ = true;
  hits_.clear();

  Serial.printf("[BLE] scan %ums (IRK-only track=%d bond=%d)\n", durationMs,
                (int)trackOn_, (int)gBleBond.hasIrk());

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
  uint32_t sec = (durationMs + 999) / 1000;
  if (sec < 2) sec = 2;
  if (sec > 15) sec = 15;

  scan->start(sec, false);
  gSink = nullptr;
  lastScanEndMs_ = millis();

  int best = -127;
  String label;
  int bondHits = 0;
  for (const auto& h : hits_) {
    if (matchHits(h)) {
      bondHits++;
      if (h.rssi > best) {
        best = h.rssi;
        label = h.name.length() ? h.name : h.addr;
      }
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

  Serial.printf("[BLE] hits=%u bond=%d match_rssi=%d label=%s miss=%u%s\n",
                (unsigned)hits_.size(), bondHits, matchRssi_,
                matchLabel_.c_str(), missStreak_, lostCar_ ? " LOST" : "");

  // 未命中时打全表，便于查 RPA/身份地址
  if (bondHits == 0) {
    for (const auto& h : hits_) {
      Serial.printf("[BLE] hit %s rssi=%d name=\"%s\"\n", h.addr.c_str(),
                    h.rssi, h.name.c_str());
    }
    Serial.printf("[BOND] irk_nonzero=%d id=%s\n",
                  gBleBond.hasIrk() ? 1 : 0, gBleBond.identityMac().c_str());
  } else {
    for (const auto& h : hits_) {
      if (matchHits(h)) {
        Serial.printf("[BLE] BOND %s rssi=%d name=\"%s\"\n", h.addr.c_str(),
                      h.rssi, h.name.c_str());
      }
    }
  }

  scan->clearResults();
  busy_ = false;
}

void BleScanTool::trackPoll(uint32_t intervalMs, uint32_t scanMs) {
  if (!trackOn_ || busy_) return;
  if (millis() < nextTrackMs_) return;
  // trackPoll 本身有 interval；scanMs 过短会被抬到 2000，热点模式允许更短
  nextTrackMs_ = millis() + intervalMs;
  if (scanMs < 1000) scanMs = 1000;
  runScan(scanMs);
}

#else
void BleScanTool::runScan(uint32_t) {
  Serial.println("[BLE] BLE not enabled in this build");
}
void BleScanTool::trackPoll(uint32_t, uint32_t) {}
std::vector<BleAdvHit> BleScanTool::matchOnlyHits() const { return {}; }
bool BleScanTool::matchHits(const BleAdvHit&) const { return false; }
#endif
