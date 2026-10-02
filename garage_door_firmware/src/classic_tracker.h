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

struct ClassicDeviceItem {
  String mac;
  int rssi;
  String name;
};

// 经典蓝牙搜索 + 目标车机 RSSI 跟踪
// 手机「可被搜索」/车机蓝牙 用经典 BT Inquiry，不是 BLE 广播
//
// 两态纪元调度（20261002）：
//  IDLE  — 每 PROBE_PERIOD_MS(8s) 一发 len=2 探针。8s 周期保证接住晨间
//          ~10s 出库信号窗（有效窗7s + 探针2.56s > 8s → 零漏检），占空比 32%。
//  ACTIVE — 开门事件（NFC/远程/自动开）或探针命中目标触发；5 次连续
//          inquiry + 3s HTTP 整窗循环（5+1）。信号消失/门关/超时退出。
//  交替只发生在纪元边界：IDLE 内 HTTP 与 inquiry 不再每 4s 交错互踩，
//  ACTIVE 内 HTTP 只在整窗出现（不跨窗打架）。
class ClassicTracker {
 public:
  bool begin(const char* macStr);
  void loop();
  // 门状态喂入（main 每轮调用）：开门沿进入 ACTIVE 纪元——晨间出库的
  // 信号窗由开门事件锚定，不依赖探针相位
  void setDoorOpen(bool open);
  bool epochActive() const { return epochActive_; }
  // ACTIVE 的 HTTP 整窗内（main 用它豁免 heapThin，见 btQuiet 计算处）
  bool inHttpWindow() const;
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
  // BTU 专用应急堆：armed 后自适应尺寸 hold；largest8<4112 或 BTU 失败时释放
  static bool btuReserveHeld();
  // staUp: STA 是否已连（由 main 传入，本模块不依赖 WiFi）
  static void serviceBtuReserve(bool staUp);
  // 我们主动钉住的堆字节数（BT air 气囊 + BTU 应急堆）。
  // main 判 heapThin 时要把它加回 maxblk：hold 是设计行为不是碎片，
  // 否则 1743 实锤——每轮 hold 把 maxblk 压到 4084 <4600 → 日志/status 被静默。
  static uint32_t pinnedHeapBytes();
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
  std::vector<ClassicDeviceItem> discoveryResults() const;

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
  // 纪元调度（实现见 .cpp 常量区）
  void enterActiveEpoch(const char* why);
  void exitActiveEpoch(const char* why);
  void openHttpWindow();
  // 实现在 .cpp（millisBefore 在 config.h）

  String targetMac_;
  bool targetSet_ = false;

  static constexpr int WIN = 10;
  int8_t hist_[WIN] = {0};
  // 采样时刻（毫秒）：slope 按真实时间归一成 dBm/s——纪元调度后采样间隔
  // 非均匀（burst 内 2.8s / 跨窗 17s），按样本序号算会随节奏漂移灵敏度
  uint32_t histMs_[WIN] = {0};
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
  std::vector<ClassicDeviceItem> discList_;

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

  // ===== 两态纪元状态 =====
  bool epochActive_ = false;      // false=IDLE(8s探针) / true=ACTIVE(5+1)
  bool epochSawSignal_ = false;   // 本纪元内是否见过目标（退出条件用）
  bool doorOpen_ = false;         // main 喂入的门态（边沿触发开门进 ACTIVE）
  uint8_t burstDone_ = 0;         // 本 burst 已完成次数（0..BURST_N-1 续扫）
  uint32_t epochStartMs_ = 0;
  uint32_t windowUntilMs_ = 0;    // ACTIVE 的 HTTP 整窗截止；0=不在窗内
  const char* epochWhy_ = "boot"; // 最近一次纪元切换原因（loop 统一打日志）
  bool epochLoggedActive_ = false; // loop 侧：epochWhy_ 是否已上送

  // 精确统计（静态，钩子/loop 共用；volatile 防优化）
  static volatile uint32_t s_inqCount;
  static volatile uint32_t s_thinCount;
  static volatile uint32_t s_btuFailCount;
  // BTU 分配失败钩子置位 → loop 释放应急堆（钩子内严禁 free）
  static volatile bool s_btuResGiveReq;
};

// OTA 专用：彻底关 BT 射频（SerialBT.end + btStop，bluedroid/controller 全拆）。
// 关掉后才能安全 WiFi.setSleep(false)（BT 开着关省电 → wifi 断言 abort）。
// 拆栈后必须 ESP.restart 才能恢复蓝牙功能。成功返回 true
bool btRadioPowerDown();
