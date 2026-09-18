#pragma once
#include <Arduino.h>
#include "config.h"

// 433MHz OOK 信号抓包：记录脉冲宽度序列，用于对比固定码/滚码
class RfCapture {
 public:
  void begin(int pin = PIN_RF_DATA);
  bool capture(uint32_t timeoutMs = RF_CAPTURE_TIMEOUT_MS);
  bool hasData() const { return count_ > 0; }
  uint16_t count() const { return count_; }
  const uint16_t* pulses() const { return pulses_; }

  // 与上一次抓包对比：true = 码相同（固定码）
  bool compareWithLast();

  void dump(uint16_t maxShow = 40) const;
  uint32_t hash() const;

 private:
  int pin_ = -1;
  uint16_t pulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t count_ = 0;

  uint16_t lastPulses_[RF_CAPTURE_MAX_PULSES];
  uint16_t lastCount_ = 0;
  bool hasLast_ = false;
};
