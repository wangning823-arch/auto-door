#pragma once
#include <Arduino.h>
#include <vector>

// 信号趋势（商品方案：只认渐变，突变不动作）
enum class SignalTrend : uint8_t {
  UNKNOWN = 0,
  GRADUAL_IN,
  GRADUAL_OUT,
  STEADY,
  SUDDEN_APPEAR,
  SUDDEN_LOSS,
};

enum class CarZone : uint8_t {
  OUT = 0,
  NEAR,
  IN_GARAGE,
  TRANSIT,
};

struct BleDeviceItem {
  String mac;
  int rssi;
  String name;
};

// 经典蓝牙搜索 + 目标车机 RSSI 跟踪
// 手机「可被搜索」/车机蓝牙 用经典 BT Inquiry，不是 BLE 广播
class BleTracker {
 public:
  bool begin(const char* macStr);
  void loop();

  // WiFi 优先：有手机连热点时降低后台 Inquiry 频率，不完全停
  void setInquiryPaused(bool paused);
  bool inquiryPaused() const { return inquiryPaused_; }
  // SoftAP 有客户端时慢速 Inquiry（保住跟踪，尽量让出射频给网页）
  void setInquirySlow(bool slow);
  void cancelActiveInquiry();
  void setAutoTrack(bool on) { autoTrack_ = on; }
  bool autoTrack() const { return autoTrack_; }

  bool hasTarget() const { return targetSet_; }
  bool seenRecently(uint32_t withinMs) const;
  // 经典 Inquiry / 用户扫描是否占用中（NFC 初始化要避开）
  bool inquiryBusy() const { return inquiryBusy_ || discRunning_; }
  // 超过 20s 未再扫到 → 返回 -127，避免网页显示卡住的旧 RSSI
  int lastRssi() const;
  int lastRssiRaw() const { return lastRssi_; }
  float slope() const { return slope_; }
  SignalTrend trend() const { return trend_; }
  CarZone zone() const { return zone_; }
  uint32_t lastSeenMs() const { return lastSeenMs_; }
  bool everInGarage() const { return everInGarage_; }
  void markLeftForCloseEval();

  void startDiscovery(uint32_t durationMs = 10000);
  bool discoveryRunning() const;
  std::vector<BleDeviceItem> discoveryResults() const;

  // GAP 回调喂入
  void onClassicDevice(const String& mac, int rssi, const String& name);
  void onDeviceName(const String& mac, const String& name);
  void onInquiryDone();

  String debugLine() const;

 private:
  void pushSample(bool visible, int rssi);
  void computeSlope();
  void classifyTrend(bool visible, int rssi);
  void updateZone();

  String targetMac_;
  bool targetSet_ = false;

  static constexpr int WIN = 10;
  int8_t hist_[WIN] = {0};
  uint8_t histCount_ = 0;
  uint8_t histHead_ = 0;

  int lastRssi_ = -127;
  float slope_ = 0;
  SignalTrend trend_ = SignalTrend::UNKNOWN;
  CarZone zone_ = CarZone::OUT;

  uint32_t lastSeenMs_ = 0;
  uint32_t silentSinceMs_ = 0;
  bool everInGarage_ = false;
  bool wasVisible_ = false;

  volatile bool discRunning_ = false;
  uint32_t discEndMs_ = 0;
  std::vector<BleDeviceItem> discList_;

  // 周期性 inquiry 用于跟踪目标
  uint32_t nextInquiryMs_ = 0;
  bool inquiryBusy_ = false;
  uint32_t inquiryStartMs_ = 0;  // busy 起点：回调丢失时超时强清
  volatile bool inquiryPaused_ = false;
  bool inquirySlow_ = false;
  bool autoTrack_ = false;
  uint8_t missCount_ = 0;
};
