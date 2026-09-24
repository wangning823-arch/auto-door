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

// BLE 只服务：配对 IRK 跟踪 RSSI（名称/MAC 特征通道已移除）
// 车机经典蓝牙走 BleTracker，不经本类
class BleScanTool {
 public:
  void runScan(uint32_t durationMs = 10000);
  bool busy() const { return busy_; }

  const std::vector<BleAdvHit>& hits() const { return hits_; }

  int matchRssi() const { return matchRssi_; }
  String matchLabel() const { return matchLabel_; }
  uint32_t lastMatchMs() const { return lastMatchMs_; }
  uint32_t lastScanEndMs() const { return lastScanEndMs_; }
  uint8_t missStreak() const { return missStreak_; }
  bool lostCar() const { return lostCar_; }
  void clearLostFlag() { lostCar_ = false; }

  // 周期跟踪：有配对 IRK 时应保持 on
  void setTrack(bool on) { trackOn_ = on; }
  bool trackOn() const { return trackOn_; }
  void trackPoll(uint32_t intervalMs = 3000, uint32_t scanMs = 1000);

  // 释放 hits_ 占用（TLS/大块分配前调用）；匹配结果已在 matchRssi_ 保留
  void releaseMemory();

  // 是否命中：仅 IRK 配对设备
  bool matchHits(const BleAdvHit& h) const;
  // 仅命中设备（给排障用，不按名称过滤）
  std::vector<BleAdvHit> matchOnlyHits() const;

 private:
  bool busy_ = false;
  bool trackOn_ = false;
  uint32_t nextTrackMs_ = 0;
  uint32_t lastScanEndMs_ = 0;
  std::vector<BleAdvHit> hits_;
  int matchRssi_ = -127;
  String matchLabel_;
  uint32_t lastMatchMs_ = 0;
  uint8_t missStreak_ = 0;
  bool lostCar_ = false;
};