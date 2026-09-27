#pragma once
#include <Arduino.h>
#include "config.h"

// 433/315MHz OOK：抓包分析 + 固定码单帧学习回放
class RfCapture {
 public:
  void begin(int rxPin = PIN_RF_DATA, int txPin = PIN_RF_TX);

  // 分析用抓包（完整序列 + 与上次对比）
  // warmupMs: 先丢弃的预热噪声；learnMode: 学习专用（更严、不许噪音提前收尾）
  bool capture(uint32_t timeoutMs = RF_CAPTURE_TIMEOUT_MS,
               uint32_t warmupMs = 0,
               bool learnMode = false);

  // 连续抓包：一直听，每收到一帧就打印 pulses 并继续；rfstop 结束
  bool captureContinuous();

  // 由 captureContinuous 内的串口轮询置位
  static void requestStop() { stopReq_ = true; }
  static bool stopRequested() { return stopReq_; }
  bool hasData() const { return count_ > 0; }
  uint16_t count() const { return count_; }
  const uint16_t* pulses() const { return pulses_; }
  bool compareWithLast();
  void dump(uint16_t maxShow = 40) const;
  uint32_t hash() const;

  // 从最近一次 capture 提取单帧（去掉帧间隔脉冲）
  bool extractOneFrame(uint16_t* out, uint16_t* outN, uint16_t maxN) const;

  // 学习：抓包 → 单帧 → 回调保存；成功返回帧长
  // saveFn(idx, pulseStr) 由上层写入 NVS
  bool learnKey(int idx, bool (*saveFn)(int, const char*));

  // 回放：pulses 逗号串；成功 true
  bool playRaw(const char* pulseCsv, uint8_t repeats = RF_PLAY_REPEATS);
  bool playFrame(const uint16_t* p, uint16_t n, uint8_t repeats = RF_PLAY_REPEATS);

  // 槽位缓存（开机加载后 playKey 直接用）
  bool setKeyFromCsv(int idx, const char* csv);
  bool playKey(int idx);
  bool keyValid(int idx) const;
  uint16_t keyCount(int idx) const;
  // 打印 RFDATA <idx> <name> <n> <csv...>
  void exportKeyCsv(int idx) const;

  // 边发边收：TX 发 idx，RX 同步抓包，对比收发波形
  // 天线靠近时可验证发射链路是否正常
  bool loopbackKey(int idx, uint8_t repeats = 2);

  // 周期回环：RX 预热后每 intervalMs 发一次，逐轮对比学习码，统计稳定性
  bool benchLoopbackKey(int idx, uint8_t rounds = 6, uint32_t intervalMs = 10000);

  // 抓包等待期间回调（rfauto 周期发射）
  static void setIdleHook(void (*fn)()) { idleHook_ = fn; }
  // 进入抓包时立刻回调（rfauto 开抓先打一帧）
  static void setStartHook(void (*fn)()) { startHook_ = fn; }

  // 发射脚自检：ms 毫秒方波（0=拉高 ms 后拉低），便于万用表测 GPIO26
  void carrierTest(uint32_t ms = 1000);

  // RX 预热后，TX 持续高电平 carrierMs，同步抓包看能否收到
  bool carrierLoopback(uint32_t carrierMs = 800);

  // 安全：强制 TX 拉低（开机/卡死恢复）
  void forceTxLow();
  // 是否正在合法发射（play/carrier 期间为 true）
  bool txBusy() const { return txBusy_; }

  int rxPin() const { return rxPin_; }
  int txPin() const { return txPin_; }

 private:
  int rxPin_ = -1;
  int txPin_ = -1;
  volatile bool txBusy_ = false;

  uint16_t pulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t count_ = 0;

  uint16_t lastPulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t lastCount_ = 0;
  bool hasLast_ = false;

  uint16_t keys_[RF_KEY_COUNT][RF_KEY_MAX_PULSES];
  uint16_t keyLen_[RF_KEY_COUNT];

  static void (*idleHook_)();
  static void (*startHook_)();
  static volatile bool stopReq_;
};
