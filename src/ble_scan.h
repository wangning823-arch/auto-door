#pragma once
#include <Arduino.h>
#include <vector>

struct BleAdvHit {
  String addr;
  String name;
  int rssi = -127;
  String services;
  String mfg;
};

// 按需 BLE 扫描 + 可选特征跟踪（名称前缀或 MAC）
class BleScanTool {
 public:
  void runScan(uint32_t durationMs = 10000);
  bool busy() const { return busy_; }

  const std::vector<BleAdvHit>& hits() const { return hits_; }
  String filter() const { return filter_; }
  void setFilter(const String& f) { filter_ = f; filter_.trim(); }

  int matchRssi() const { return matchRssi_; }
  String matchLabel() const { return matchLabel_; }
  uint32_t lastMatchMs() const { return lastMatchMs_; }
  uint8_t missStreak() const { return missStreak_; }
  bool lostCar() const { return lostCar_; }
  void clearLostFlag() { lostCar_ = false; }

  // 周期跟踪：scanMs 建议 800–1500；intervalMs 3000+
  void setTrack(bool on) { trackOn_ = on; }
  bool trackOn() const { return trackOn_; }
  void trackPoll(uint32_t intervalMs = 3000, uint32_t scanMs = 1000);

  // 给 Web 用：仅 MiCar / fcd1 / 指定前缀 列表（按 RSSI 降序）
  std::vector<BleAdvHit> interestingHits() const;
  bool matchHits(const BleAdvHit& h) const;

 private:
  bool busy_ = false;
  bool trackOn_ = false;
  uint32_t nextTrackMs_ = 0;
  uint32_t lastScanEndMs_ = 0;
  std::vector<BleAdvHit> hits_;
  String filter_;
  int matchRssi_ = -127;
  String matchLabel_;
  uint32_t lastMatchMs_ = 0;
  uint8_t missStreak_ = 0;
  bool lostCar_ = false;
};
