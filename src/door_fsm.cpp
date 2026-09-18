#include "door_fsm.h"
#include "config.h"

void DoorFsm::begin() {
  relayPin_ = PIN_RELAY;
  // 开漏：低=拉到地吸合；高=引脚悬空释放（5V光耦板悬空=灭，无需三极管）
  pinMode(relayPin_, OUTPUT_OPEN_DRAIN);
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPin_, HIGH);  // 悬空释放
#else
  digitalWrite(relayPin_, LOW);
#endif
  pinMode(PIN_DOOR_MAGNET, INPUT_PULLUP);
  pinMode(PIN_TRIG_IN, INPUT_PULLUP);
  pinMode(PIN_LEARN_BTN, INPUT_PULLUP);
  pinMode(PIN_STATUS_LED, OUTPUT);
  doorState_ = DoorState::UNKNOWN;
  Serial.printf("[FSM] begin (F0, relay pin=%d active_%s OD)\n", relayPin_,
                RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
}

void DoorFsm::setRelayPin(int pin) {
  if (pin < 0 || pin > 39) return;
  if (pin == 0 || pin == 1 || pin == 3 || pin == PIN_DOOR_MAGNET ||
      pin == PIN_TRIG_IN || pin == PIN_STATUS_LED) {
    Serial.println("[FSM] relay pin reserved, try 21/13/32/33");
    return;
  }
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPin_, HIGH);
#else
  digitalWrite(relayPin_, LOW);
#endif
  relayPin_ = pin;
  pinMode(relayPin_, OUTPUT_OPEN_DRAIN);
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPin_, HIGH);
#else
  digitalWrite(relayPin_, LOW);
#endif
  Serial.printf("[FSM] relay pin -> %d (open-drain)\n", relayPin_);
}

void DoorFsm::pulseRelay() {
  Serial.printf("[FSM] RELAY PULSE %dms pin=%d\n", RELAY_PULSE_MS, relayPin_);
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPin_, LOW);
  delay(RELAY_PULSE_MS);
  digitalWrite(relayPin_, HIGH);
#else
  digitalWrite(relayPin_, HIGH);
  delay(RELAY_PULSE_MS);
  digitalWrite(relayPin_, LOW);
#endif
  lastAnyActionTs_ = millis();
}

void DoorFsm::emitOpen() {
  if (rfEmit_ && rfEmit_(true)) {
    Serial.println("[FSM] RF TX open/up");
    lastAnyActionTs_ = millis();
    return;
  }
  pulseRelay();
}

void DoorFsm::emitClose() {
  if (rfEmit_ && rfEmit_(false)) {
    Serial.println("[FSM] RF TX close/down");
    lastAnyActionTs_ = millis();
    return;
  }
  pulseRelay();
}

bool DoorFsm::canAutoOpenNow() const {
  if (holdOpen_) return false;
  if (doorState_ == DoorState::OPEN) return false;
  uint32_t now = millis();
  if (lastAutoOpenTs_ && (now - lastAutoOpenTs_) < AUTO_COOLDOWN_OPEN_MS) return false;
  return true;
}

bool DoorFsm::canAutoCloseNow() const {
  if (holdOpen_) return false;
  if (doorState_ == DoorState::CLOSED) return false;
  uint32_t now = millis();
  if (lastAutoCloseTs_ && (now - lastAutoCloseTs_) < AUTO_COOLDOWN_CLOSE_MS) return false;
  if (lastAutoOpenTs_ && (now - lastAutoOpenTs_) < AUTO_MIN_OPEN_HOLD_MS) return false;
  return true;
}

bool DoorFsm::tryAutoOpen(const char* why) {
  if (!canAutoOpenNow()) return false;
  Serial.printf("[FSM] AUTO OPEN (%s)\n", why ? why : "");
  pending_ = DoorAction::PULSE_OPEN;
  openSource_ = OpenSource::AUTO;
  doorState_ = DoorState::OPEN;
  doorOpenTs_ = millis();
  lastAutoOpenTs_ = millis();
  emitOpen();
  pending_ = DoorAction::NONE;
  return true;
}

bool DoorFsm::tryAutoClose(const char* why) {
  if (!canAutoCloseNow()) return false;
  Serial.printf("[FSM] AUTO CLOSE (%s)\n", why ? why : "");
  pending_ = DoorAction::PULSE_CLOSE;
  emitClose();
  doorState_ = DoorState::CLOSED;
  openSource_ = OpenSource::NONE;
  doorOpenTs_ = 0;
  lastAutoCloseTs_ = millis();
  pending_ = DoorAction::NONE;
  return true;
}

void DoorFsm::requestManualToggle(OpenSource src) {
  // 无门磁时状态不可靠：UNKNOWN 也走「开」——网页请用明确的开/关按钮
  if (doorState_ == DoorState::OPEN) {
    requestManualClose(src);
  } else {
    requestManualOpen(src);
  }
}

void DoorFsm::requestManualOpen(OpenSource src) {
  Serial.println("[FSM] MANUAL OPEN");
  pending_ = DoorAction::PULSE_OPEN;
  emitOpen();
  doorState_ = DoorState::OPEN;
  openSource_ = src;
  doorOpenTs_ = millis();
  pending_ = DoorAction::NONE;
  lastAnyActionTs_ = millis();
}

void DoorFsm::requestManualClose(OpenSource src) {
  Serial.println("[FSM] MANUAL CLOSE");
  pending_ = DoorAction::PULSE_CLOSE;
  emitClose();
  doorState_ = DoorState::CLOSED;
  openSource_ = src == OpenSource::NONE ? OpenSource::NONE : src;
  doorOpenTs_ = 0;
  pending_ = DoorAction::NONE;
  lastAnyActionTs_ = millis();
}

void DoorFsm::notifyMagnet(bool closed) {
  magnetOk_ = true;
  DoorState s = closed ? DoorState::CLOSED : DoorState::OPEN;
  if (s != doorState_) {
    // 若原遥控开关，标记 UNKNOWN 来源（仅当与系统状态不一致）
    if (doorState_ != DoorState::UNKNOWN &&
        openSource_ == OpenSource::NONE) {
      openSource_ = OpenSource::UNKNOWN_SRC;
    }
    doorState_ = s;
    if (s == DoorState::OPEN) doorOpenTs_ = millis();
    if (s == DoorState::CLOSED) doorOpenTs_ = 0;
    Serial.printf("[FSM] magnet -> %s\n", s == DoorState::OPEN ? "OPEN" : "CLOSED");
  }
}

void DoorFsm::loop(BleTracker& bt) {
  // 门磁
  static uint32_t lastMag = 0;
  if (millis() - lastMag > 200) {
    lastMag = millis();
    bool closed = digitalRead(PIN_DOOR_MAGNET) == LOW;
    notifyMagnet(closed);
  }

  // TRIG = 米家插座路径
  static bool lastTrig = true;
  bool trig = digitalRead(PIN_TRIG_IN);
  if (lastTrig && !trig) {
    Serial.println("[FSM] TRIG edge -> MIAO toggle");
    requestManualToggle(OpenSource::MIAO);
  }
  lastTrig = trig;

  // 学习键短按 = 手动开关（P0）
  static bool lastBtn = true;
  bool btn = digitalRead(PIN_LEARN_BTN);
  static uint32_t btnDown = 0;
  if (lastBtn && !btn) btnDown = millis();
  if (!lastBtn && btn && btnDown && (millis() - btnDown) < 1500 &&
      (millis() - btnDown) > 30) {
    Serial.println("[FSM] LEARN BTN short -> toggle");
    requestManualToggle(OpenSource::NFC);
  }
  lastBtn = btn;

  // 自动开/关由 main.cpp 的 BLE 状态机驱动，这里不再评估
  digitalWrite(PIN_STATUS_LED, doorState_ == DoorState::OPEN ? HIGH : LOW);
}

DoorAction DoorFsm::consumeAction() {
  DoorAction a = pending_;
  pending_ = DoorAction::NONE;
  return a;
}

String DoorFsm::debugLine() const {
  char buf[128];
  snprintf(buf, sizeof(buf), "door=%d src=%d open_ts=%lu hold=%d",
           (int)doorState_, (int)openSource_, (unsigned long)doorOpenTs_,
           holdOpen_ ? 1 : 0);
  return String(buf);
}
