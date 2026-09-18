#pragma once
#include <Arduino.h>
#include "ble_tracker.h"
#include "config.h"

enum class DoorState : uint8_t { UNKNOWN = 0, CLOSED, OPEN };
enum class OpenSource : uint8_t {
  NONE = 0,
  AUTO,
  NFC,
  MIAO,
  REMOTE,
  UNKNOWN_SRC,
};

enum class DoorAction : uint8_t { NONE = 0, PULSE_OPEN, PULSE_CLOSE };

class DoorFsm {
 public:
  // RF 发射回调：open=true 发「开/上」键，false 发「关/下」键；返回是否已发
  using RfEmitFn = bool (*)(bool open);

  void begin();
  void loop(BleTracker& bt);

  DoorState doorState() const { return doorState_; }
  OpenSource openSource() const { return openSource_; }

  void setRfEmit(RfEmitFn fn) { rfEmit_ = fn; }

  // 外部触发：NFC / TRIG(米家) / 串口
  void requestManualToggle(OpenSource src);
  void notifyMagnet(bool closed);
  void setHoldOpen(bool hold) { holdOpen_ = hold; }

  // 简化自动开/关（不依赖门磁，只发脉冲）
  bool tryAutoOpen(const char* why);
  bool tryAutoClose(const char* why);
  bool canAutoOpenNow() const;
  bool canAutoCloseNow() const;
  uint32_t lastAutoOpenTs() const { return lastAutoOpenTs_; }
  uint32_t lastAutoCloseTs() const { return lastAutoCloseTs_; }

  // 运行时改继电器脚（串口 pin 21/26/13/...）
  void setRelayPin(int pin);
  int relayPin() const { return relayPin_; }

  DoorAction consumeAction();
  String debugLine() const;

 private:
  void pulseRelay();
  void emitOpen();
  void emitClose();

  int relayPin_ = PIN_RELAY;
  RfEmitFn rfEmit_ = nullptr;

  DoorState doorState_ = DoorState::UNKNOWN;
  OpenSource openSource_ = OpenSource::NONE;
  DoorAction pending_ = DoorAction::NONE;

  uint32_t doorOpenTs_ = 0;
  uint32_t lastAutoOpenTs_ = 0;
  uint32_t lastAutoCloseTs_ = 0;
  uint32_t lastAnyActionTs_ = 0;
  bool holdOpen_ = false;
  bool magnetOk_ = false;
};
