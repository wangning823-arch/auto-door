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
  // 是否已真正 init（网页扫描前判断，避免未起栈就 Inquiry）
  bool ready() const { return ready_; }

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
  // 方向A：inquiry 中或刚结束后保护窗 → log/status 不发大包（poll 仍可试）
  bool btQuietForHttp() const;

  // ===== 精确堆/BTU 统计（不依赖 HEAPFAIL 抽样）=====
  // inquiry 次数 / thin 次数 / BTU 4112 失败次数（分配钩子里累加）
  static uint32_t inqCount() { return s_inqCount; }
  static uint32_t thinCount() { return s_thinCount; }
  static uint32_t btuFailCount() { return s_btuFailCount; }
  // onAllocFailed 里调用：size/task 判定 BTU 失败，仅计数不分配
  static void noteAllocFail(size_t size, const char* task);
  static void noteThin() { s_thinCount++; }
  static void noteInquiryStart() { s_inqCount++; }
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

  // RSSI 时间序列（网页曲线）：环形缓存，约 180 点
  static constexpr int TS_N = 180;
  void recordTs(int16_t rssi);  // <=-127 表示无信号
  void clearTs();
  int tsCount() const { return tsCount_; }
  int tsExport(uint32_t* tSec, int16_t* rssi, int maxN) const;

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

  uint32_t tsMs_[TS_N];
  int16_t tsRssi_[TS_N];
  uint16_t tsHead_ = 0;
  uint16_t tsCount_ = 0;

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
  // inquiry 结束后保护窗截止（BTU 异步 malloc 窗口）
  uint32_t postQuietUntilMs_ = 0;
  volatile bool inquiryPaused_ = false;
  bool inquirySlow_ = false;
  bool autoTrack_ = false;
  uint8_t missCount_ = 0;
  bool ready_ = false;

  // 精确统计（静态，钩子/loop 共用；volatile 防优化）
  static volatile uint32_t s_inqCount;
  static volatile uint32_t s_thinCount;
  static volatile uint32_t s_btuFailCount;
};

// OTA 专用：彻底关 BT 射频（SerialBT.end + btStop，bluedroid/controller 全拆）。
// 关掉后才能安全 WiFi.setSleep(false)（BT 开着关省电 → wifi 断言 abort）。
// 拆栈后必须 ESP.restart 才能恢复蓝牙功能。成功返回 true
bool btRadioPowerDown();
