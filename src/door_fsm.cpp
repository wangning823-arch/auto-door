#include "door_fsm.h"
#include "config.h"

// millisReached 在 config.h

void DoorFsm::begin() {
  relayPin_ = PIN_RELAY;
  // 开漏：低=拉到地吸合；高=引脚悬空释放（5V光耦板悬空=灭，无需三极管）
  pinMode(relayPin_, OUTPUT_OPEN_DRAIN);
#if RELAY_ACTIVE_LOW
  digitalWrite(relayPin_, HIGH);  // 悬空释放
#else
  digitalWrite(relayPin_, LOW);
#endif
  pinMode(PIN_TRIG_IN, INPUT_PULLUP);
  pinMode(PIN_LEARN_BTN, INPUT_PULLUP);
  pinMode(PIN_STATUS_LED, OUTPUT);
  // 开/关为不同 RF 码，不用门磁推断门态
  doorState_ = DoorState::UNKNOWN;
  Serial.printf("[FSM] begin (F0, relay pin=%d active_%s OD)\n", relayPin_,
                RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
}

void DoorFsm::setRelayPin(int pin) {
  if (pin < 0 || pin > 39) return;
  if (pin == 0 || pin == 1 || pin == 3 ||
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
    lastAnyActionTs_ = millis();
    if (Serial.availableForWrite() > 32)
      Serial.println("[FSM] RF TX open/up");
    return;
  }
  pulseRelay();
}

void DoorFsm::emitClose() {
  if (rfEmit_ && rfEmit_(false)) {
    lastAnyActionTs_ = millis();
    if (Serial.availableForWrite() > 32)
      Serial.println("[FSM] RF TX close/down");
    return;
  }
  pulseRelay();
}

bool DoorFsm::canAutoOpenNow() const {
  if (holdOpen_) return false;
  // 手动操作后：只挡自动开（防手动关完又被顶开），不挡离场自动关
  if (suppressAutoOpenUntil_ && !millisReached(millis(), suppressAutoOpenUntil_))
    return false;
  // 不因「门已开/上次开过很久」拒绝：进场一律可再发开码（门已开再开一次无害）
  return true;
}

bool DoorFsm::canAutoCloseNow() const {
  if (holdOpen_) return false;
  // 不看 doorState、不看开后 hold、不看关冷却：
  // 离场判定成立就发关码（门其实关着再关一次也无妨）
  return true;
}

bool DoorFsm::tryAutoOpen(const char* why) {
  if (!canAutoOpenNow()) {
    Serial.printf("[FSM] AUTO OPEN 拒绝 (%s) hold=%d suppress=%d\n",
                  why ? why : "?", holdOpen_ ? 1 : 0,
                  suppressAutoOpenUntil_ &&
                          !millisReached(millis(), suppressAutoOpenUntil_)
                      ? 1
                      : 0);
    return false;
  }
  Serial.printf("[FSM] AUTO OPEN (%s)\n", why ? why : "");
  pending_ = DoorAction::PULSE_OPEN;
  openSource_ = OpenSource::AUTO;
  doorState_ = DoorState::OPEN;
  doorOpenTs_ = millis();
  lastAutoOpenTs_ = millis();
  lastCmd_ = LastCmd::OPEN;
  emitOpen();
  pending_ = DoorAction::NONE;
  return true;
}

bool DoorFsm::tryAutoClose(const char* why) {
  if (!canAutoCloseNow()) {
    Serial.printf("[FSM] AUTO CLOSE 拒绝 (%s) hold=%d\n", why ? why : "?",
                  holdOpen_ ? 1 : 0);
    return false;
  }
  Serial.printf("[FSM] AUTO CLOSE (%s)\n", why ? why : "");
  pending_ = DoorAction::PULSE_CLOSE;
  emitClose();
  doorState_ = DoorState::CLOSED;
  openSource_ = OpenSource::NONE;
  doorOpenTs_ = 0;
  lastAutoCloseTs_ = millis();
  lastCmd_ = LastCmd::CLOSE;
  pending_ = DoorAction::NONE;
  return true;
}

void DoorFsm::requestManualToggle(OpenSource src) {
  // 不以 doorState 为唯一依据：软件门态可能与真实门不一致
  // 以「上次本机发出的 RF 指令」翻转：上次开→这次必发关；上次关/未知→发开
  bool sendClose = (lastCmd_ == LastCmd::OPEN) ||
                   (lastCmd_ == LastCmd::NONE && doorState_ == DoorState::OPEN);
  if (sendClose) {
    requestManualClose(src);
  } else {
    requestManualOpen(src);
  }
}

void DoorFsm::requestManualOpen(OpenSource src) {
  // 先发射再打日志：Serial 缓冲满时 println 会卡住 loop，RF 就发不出去
  pending_ = DoorAction::PULSE_OPEN;
  emitOpen();
  doorState_ = DoorState::OPEN;
  openSource_ = src;
  doorOpenTs_ = millis();
  lastCmd_ = LastCmd::OPEN;
  pending_ = DoorAction::NONE;
  lastAnyActionTs_ = millis();
  // 只挡后续自动开，不挡离场自动关
  suppressAutoOpenUntil_ = millis() + MANUAL_SUPPRESS_MS;
  Serial.println("[FSM] MANUAL OPEN done");
}

void DoorFsm::requestManualClose(OpenSource src) {
  pending_ = DoorAction::PULSE_CLOSE;
  emitClose();
  doorState_ = DoorState::CLOSED;
  openSource_ = src == OpenSource::NONE ? OpenSource::NONE : src;
  doorOpenTs_ = 0;
  lastCmd_ = LastCmd::CLOSE;
  pending_ = DoorAction::NONE;
  lastAnyActionTs_ = millis();
  suppressAutoOpenUntil_ = millis() + MANUAL_SUPPRESS_MS;
  Serial.println("[FSM] MANUAL CLOSE done");
}

void DoorFsm::loop(BleTracker& bt) {
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
