#pragma once
#include <Arduino.h>
#include "config.h"

// 433/315MHz OOK：抓包分析 + 固定码单帧学习回放
class RfCapture {
 public:
  void begin(int rxPin = PIN_RF_DATA, int txPin = PIN_RF_TX);

  // 分析用抓包（完整序列 + 与上次对比）
  bool capture(uint32_t timeoutMs = RF_CAPTURE_TIMEOUT_MS);
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

  int rxPin() const { return rxPin_; }
  int txPin() const { return txPin_; }

 private:
  int rxPin_ = -1;
  int txPin_ = -1;

  uint16_t pulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t count_ = 0;

  uint16_t lastPulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t lastCount_ = 0;
  bool hasLast_ = false;

  uint16_t keys_[RF_KEY_COUNT][RF_KEY_MAX_PULSES];
  uint16_t keyLen_[RF_KEY_COUNT];
};
