#include "nfc_reader.h"
#include "config.h"
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_PN532.h>
#include <driver/gpio.h>

// 16/17 引脚定义不变（SDA=16 SCL=17）。0xFF=不用库的 IRQ/RESET 脚。
static Adafruit_PN532 nfc((uint8_t)0xFF, (uint8_t)0xFF);

// 尽早拉高 SDA/SCL：模块与 ESP 同电，上电瞬间 SCL 必须为高，否则芯片易卡死
static void earlyBusIdleHigh() {
  pinMode(PIN_NFC_SDA, INPUT_PULLUP);
  pinMode(PIN_NFC_SCL, INPUT_PULLUP);
}
static bool gEarlyBus = (earlyBusIdleHigh(), true);

// Wire.begin 后 ESP 仅开漏、默认不启用内部上拉；无外挂上拉时线会漂到负值再掉到 0.04
static void forceIdlePullups(int sda, int scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  if (sda >= 0)
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
  if (scl >= 0)
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);
}

#define NFC_COOLDOWN_MS 250   // 同一张卡防连读；刷开后再刷关无需长等待
#define NFC_POLL_MIN_MS 350
#define NFC_RECOVER_GAP_MS 15000
#define NFC_INIT_DELAY_MS 8000
#define NFC_FAIL_BEFORE_RESYNC 3
// 上电自动 init：给 WiFi/BT 起完再碰 I2C，避免和启动抢总线
#define NFC_BOOT_INIT_DELAY_MS 5000
// 失败后慢速自动重试（防止 15s 级 Error263 风暴锁死 SCL）
#define NFC_AUTO_RETRY_GAP_MS (10UL * 60UL * 1000UL)
#define NFC_AUTO_RETRY_MAX 20

static void i2cBusRecover(int sda, int scl) {
  // 先推拉顶一下：PN532 从机拉住 SCL 时，仅开漏 9-clock 顶不开
  pinMode(scl, OUTPUT);
  digitalWrite(scl, HIGH);
  delay(2);
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);
  delayMicroseconds(50);
  pinMode(sda, INPUT_PULLUP);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 9; i++) {
      digitalWrite(scl, LOW);
      delayMicroseconds(80);
      digitalWrite(scl, HIGH);
      delayMicroseconds(80);
    }
    if (digitalRead(sda) && digitalRead(scl)) break;
    delay(3);
  }
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW);
  delayMicroseconds(80);
  digitalWrite(sda, HIGH);
  delayMicroseconds(80);
  forceIdlePullups(sda, scl);
  delay(5);
}

static bool busIdle(int sda, int scl) {
  forceIdlePullups(sda, scl);
  delay(2);
  return digitalRead(sda) && digitalRead(scl);
}

// I2C 超时后必须松手：否则 ESP 外设/从机时钟拉伸会把 SCL 按在 0.04
static void releaseBus(int sda, int scl) {
  Wire.end();
  forceIdlePullups(sda, scl);
  delay(2);
}

// init 失败统一收尾：松手 + 计失败（禁止后台自动再撞，只允许手动 nfcinit）
static void failRelease(const char* why, int sda, int scl, uint16_t* streak) {
  Serial.printf("[NFC] FAIL %s SCL=%d → Wire.end\n", why, digitalRead(scl));
  releaseBus(sda, scl);
  if (*streak < 60000) (*streak)++;
}

// 仅诊断用：自拼 RFConfiguration。正常路径勿调（失败会留脏帧）
static bool pn532Cmd(uint8_t* cmd, uint8_t cmdlen, uint16_t timeout) {
  if (!nfc.sendCommandCheckAck(cmd, cmdlen, timeout)) return false;
  uint8_t resp[16] = {0};
  nfc.readResponse(resp, 6);
  return true;
}

// 丢掉 PN532 还没读走的响应：只认 ready 字节，禁止盲读 32 字节（会撞 Error 263）
static void pn532Drain() {
  const uint8_t addr = PN532_I2C_ADDRESS;
  // 保存真实超时再恢复：写死 200 会把 init 期的 1000 误改短，或把 50 固化
  uint16_t oldTo = 200;
#if defined(ESP32)
  oldTo = (uint16_t)Wire.getTimeOut();
#endif
  Wire.setTimeOut(50);  // drain 要快失败，别拖 500ms
  for (int i = 0; i < 4; i++) {
    uint8_t rdy = 0;
    uint8_t n = Wire.requestFrom((uint8_t)addr, (uint8_t)1);
    if (n == 1 && Wire.available()) {
      rdy = Wire.read();
    } else {
      break;
    }
    if (rdy != PN532_I2C_READY) break;
    uint8_t got = Wire.requestFrom((uint8_t)addr, (uint8_t)16);
    uint8_t seen = 0;
    while (Wire.available() && seen < got) {
      Wire.read();
      seen++;
    }
  }
  Wire.setTimeOut(oldTo ? oldTo : 200);
}

// 重开 I2C：RF/长超时后外设状态脏，残留 RDY 会让下一条 ACK 等到 1.3s
static void nfcRewire(int sda, int scl) {
  releaseBus(sda, scl);
  delay(25);
  Wire.begin(sda, scl, (uint32_t)100000);
  Wire.setTimeOut(200);
  forceIdlePullups(sda, scl);
  delay(15);
}

// 持续开 RF 场：setRetries 后先 drain 再发；失败 rewire 后重试一次
static bool pn532RfFieldOn(int sda, int scl) {
  uint8_t rfOn[3] = {0x32, 0x01, 0x01};
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt == 0) {
      pn532Drain();  // setRetries 响应可能比库读的 6 字节长
      delay(20);
    } else {
      nfcRewire(sda, scl);
      pn532Drain();
      delay(30);
    }
    if (nfc.sendCommandCheckAck(rfOn, 3, 800)) {
      uint8_t resp[12] = {0};
      nfc.readResponse(resp, 10);
      Serial.printf("[NFC] RF resp %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    resp[0], resp[1], resp[2], resp[3], resp[4], resp[5],
                    resp[6], resp[7]);
      nfcRewire(sda, scl);  // 读完再干净开下一条 poll
      return true;
    }
    Serial.printf("[NFC] RF field try%d ack=0\n", attempt);
  }
  nfcRewire(sda, scl);
  return false;
}

// 读卡连续失败后的总线复活：不整颗 nfc.begin（避免和成功路径打架）
bool NfcReader::recoverBusAndResync() {
  uint32_t now = millis();
  if (now - lastResyncMs_ < 3000) return false;
  lastResyncMs_ = now;
  Serial.println("[NFC] resync bus + SAMConfig");

  releaseBus(sda_, scl_);
  i2cBusRecover(sda_, scl_);
  if (!busIdle(sda_, scl_)) {
    failRelease("resync idle", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }
  Wire.begin(sda_, scl_, (uint32_t)100000);
  Wire.setTimeOut(1000);
  delay(50);

  if (!nfc.SAMConfig()) {
    failRelease("resync SAM", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }
  if (!busIdle(sda_, scl_)) {
    failRelease("resync after SAM", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }
  {
    bool retriesOk = nfc.setPassiveActivationRetries(0x04);
    if (!retriesOk) {
      failRelease("resync retries", sda_, scl_, &failStreak_);
      ok_ = false;
      deferred_ = true;
      return false;
    }
  }
  delay(30);
  bool rfOk = pn532RfFieldOn(sda_, scl_);
  if (!rfOk) {
    Serial.println("[NFC] resync RF field 仍失败，继续（靠 InList 开场）");
  }
  if (!busIdle(sda_, scl_)) {
    failRelease("resync after RF", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }
  failStreak_ = 0;
  ok_ = true;
  deferred_ = false;
  lastFieldMs_ = millis();
  Serial.printf("[NFC] resync OK rf=%d\n", (int)rfOk);
  return true;
}

// 与下午能出 0x32010607 的路径一致：完整 PN532 命令，不做裸 0x24 探测
bool NfcReader::hwInit() {
  if (sda_ < 0) return false;
  Serial.printf("[NFC] hwInit t=%ums\n", (unsigned)millis());

  releaseBus(sda_, scl_);
  if (!busIdle(sda_, scl_)) {
    Serial.println("[NFC] bus low → recover first");
    i2cBusRecover(sda_, scl_);
  }
  int idleSda = digitalRead(sda_);
  int idleScl = digitalRead(scl_);
  Serial.printf("[NFC] idle SDA=%d SCL=%d (t=%ums)\n", idleSda, idleScl,
                (unsigned)millis());
  // 总线仍被拉死时禁止 nfc.begin/getFirmwareVersion（会 1s 超时连打卡死 loop）
  if (!idleSda || !idleScl) {
    Serial.println("[NFC] abort: bus not idle, will not touch PN532 cmds");
    ok_ = false;
    deferred_ = true;
    if (failStreak_ < 60000) failStreak_++;
    return false;
  }

  // begin() 后芯片可能仍在 SAMConfig 忙，先松手再读 ver，避免首读固定 1.3s 超时
  releaseBus(sda_, scl_);
  delay(50);
  Wire.begin(sda_, scl_, (uint32_t)100000);
  Wire.setTimeOut(1000);
  gpio_set_pull_mode((gpio_num_t)sda_, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode((gpio_num_t)scl_, GPIO_PULLUP_ONLY);
  delay(50);

  uint32_t t0 = millis();
  nfc.begin();  // 内部 wakeup→SAMConfig，可能拉伸
  delay(300);   // 给 SAMConfig/时钟拉伸收尾时间
  if (!busIdle(sda_, scl_)) {
    failRelease("begin/SAMConfig", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }

  uint32_t ver = nfc.getFirmwareVersion();
  Serial.printf("[NFC] ver=0x%08X cost=%ums SCL=%d\n", ver,
                (unsigned)(millis() - t0), digitalRead(scl_));

  if (!ver) {
    Serial.println("[NFC] ver=0 → recover + retry once");
    releaseBus(sda_, scl_);
    i2cBusRecover(sda_, scl_);
    if (!busIdle(sda_, scl_)) {
      failRelease("ver retry idle", sda_, scl_, &failStreak_);
      ok_ = false;
      deferred_ = true;
      return false;
    }
    Wire.begin(sda_, scl_, (uint32_t)100000);
    Wire.setTimeOut(1000);
    delay(200);
    nfc.begin();
    delay(200);
    ver = nfc.getFirmwareVersion();
    Serial.printf("[NFC] retry ver=0x%08X SCL=%d\n", ver, digitalRead(scl_));
  }

  if (!ver) {
    failRelease("ver=0", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }

  // begin() 内已 SAMConfig；用库函数设重试（响应长度与官方例程一致）
  {
    bool ack = nfc.setPassiveActivationRetries(0x04);
    Serial.printf("[NFC] setRetries ack=%d SCL=%d\n", (int)ack,
                  digitalRead(scl_));
    if (!ack || !busIdle(sda_, scl_)) {
      failRelease("setRetries", sda_, scl_, &failStreak_);
      ok_ = false;
      deferred_ = true;
      return false;
    }
  }
  delay(50);

  // 持续 RF 场：手机贴卡靠场常开；失败非致命但必须 rewire，否则 poll 卡 1.3s
  {
    bool rf = pn532RfFieldOn(sda_, scl_);
    Serial.printf("[NFC] RF field on ack=%d SCL=%d\n", (int)rf,
                  digitalRead(scl_));
    if (!busIdle(sda_, scl_)) {
      failRelease("RF field bus", sda_, scl_, &failStreak_);
      ok_ = false;
      deferred_ = true;
      return false;
    }
    if (!rf) {
      Serial.println("[NFC] RF field 单独失败，继续 ready（poll 会开）");
    }
  }

  if (!busIdle(sda_, scl_)) {
    failRelease("post-init", sda_, scl_, &failStreak_);
    ok_ = false;
    deferred_ = true;
    return false;
  }

  // 100ms 太短：readPassiveTargetID 等待时会先撞 I2C 超时（Error263）
  // 500ms 会把 poll 拖到 1.3s；RF 常开后 InList 应很快返回
  Wire.setTimeOut(200);
  ok_ = true;
  deferred_ = false;
  listen_ = true;
  listenUntilMs_ = 0;
  failStreak_ = 0;
  lastOkMs_ = millis();
  nextPollMs_ = millis() + 200;
  lastFieldMs_ = millis();
  emptyPolls_ = 0;
  slowAckStreak_ = 0;
  lastPollSlow_ = false;
  Serial.printf("[NFC] PN532 ready 0x%08X\n", ver);
  return true;
}

bool NfcReader::begin(int sda, int scl) {
  sda_ = sda;
  scl_ = scl;
  ok_ = false;
  // 不再上电即永久 deferred：排程一次自动 init，断电重启后刷卡可自恢复
  deferred_ = false;
  bootInitDone_ = false;
  bootInitAt_ = millis() + NFC_BOOT_INIT_DELAY_MS;
  lastAutoRetryMs_ = 0;
  autoRetryCount_ = 0;
  listen_ = false;  // hwInit 成功后自动 listen
  bootPostpones_ = 0;
  nextPollMs_ = millis() + 200;
  lastRecoverMs_ = millis() - NFC_RECOVER_GAP_MS;
  forceIdlePullups(sda_, scl_);
  Serial.printf("[NFC] setup：硬件初始化已排程，约 %ums 后自动 nfcinit\n",
                (unsigned)NFC_BOOT_INIT_DELAY_MS);
  return false;
}

void NfcReader::postponeBootInit(uint32_t delayMs) {
  if (ok_ || bootInitDone_) return;
  uint32_t at = millis() + delayMs;
  if ((int32_t)(at - bootInitAt_) > 0) bootInitAt_ = at;
}

// 关热点后立刻重试：否则上次 hwInit 失败会卡在 10 分钟自动重试里
void NfcReader::kickRecover() {
  if (ok_) return;
  bootInitAt_ = millis();
  lastAutoRetryMs_ = millis() - NFC_AUTO_RETRY_GAP_MS;
  lastRecoverMs_ = millis() - NFC_RECOVER_GAP_MS;
  nextPollMs_ = millis();
  // 未成功过：强制重走上电 init 路径
  if (!bootInitDone_) {
    deferred_ = false;
  }
  Serial.printf("[NFC] kickRecover defer=%d bootDone=%d\n", (int)deferred_,
                (int)bootInitDone_);
}

void NfcReader::maybeRecover() {
  uint32_t now = millis();
  if (ok_) return;

  // 1) 上电自动 init（一次性；成功即恢复刷卡）
  if (!bootInitDone_) {
    // 热点有人：最多推迟 3 次×1.5s（给 HTTP 稳一下），之后必须 init
    // 否则「连热点配 Wi‑Fi」会把上电 init 无限延后 → NFC 永远起不来
    if (WiFi.softAPgetStationNum() > 0 && bootPostpones_ < 3) {
      bootPostpones_++;
      bootInitAt_ = now + 1500;
      return;
    }
    if (!millisReached(now, bootInitAt_)) return;
    bootInitDone_ = true;
    Serial.printf("[NFC] 上电自动 init t=%ums SCL=%d\n", (unsigned)now,
                  digitalRead(scl_ >= 0 ? scl_ : PIN_NFC_SCL));
    if (hwInit()) {
      Serial.println("[NFC] 上电自动 init OK");
      return;
    }
    // hwInit 失败路径已 deferred_=true
    lastAutoRetryMs_ = now;
    autoRetryCount_ = 1;
    Serial.println("[NFC] 上电自动 init 失败 → 进入慢速自动重试");
    return;
  }

  // 2) deferred：慢速有限次重试，避免 Error263 风暴；超限后仅串口/网页 forceInit
  //    OTA/STA 开着时上电首次 init 更容易被 WiFi 时序带失败 → 前几次改 30s 快重试
  if (deferred_) {
    if (autoRetryCount_ >= NFC_AUTO_RETRY_MAX) return;
    uint32_t gap = (autoRetryCount_ <= 3) ? 30000UL : NFC_AUTO_RETRY_GAP_MS;
    if (!millisReached(now, lastAutoRetryMs_ + gap)) return;
    lastAutoRetryMs_ = now;
    autoRetryCount_++;
    Serial.printf("[NFC] 自动重试 %u/%u gap=%ums t=%ums fail=%u\n",
                  autoRetryCount_, (unsigned)NFC_AUTO_RETRY_MAX, (unsigned)gap,
                  (unsigned)now, failStreak_);
    if (hwInit()) {
      autoRetryCount_ = 0;
      Serial.println("[NFC] 自动重试成功");
    }
    // 失败时 hwInit 保持 deferred_=true
    return;
  }

  // 3) 非 deferred 且未 ok：运行中总线恢复节流
  if (!millisReached(now, lastRecoverMs_ + NFC_RECOVER_GAP_MS)) {
    return;
  }
  lastRecoverMs_ = now;
  Serial.printf("[NFC] recover try fail=%u\n", failStreak_);
  if (!hwInit()) {
    deferred_ = true;
    lastAutoRetryMs_ = now;
  }
}

bool NfcReader::forceInit() {
  lastRecoverMs_ = millis();
  bootInitDone_ = true;  // 手动初始化后不必再等上电排程
  Serial.printf("[NFC] forceInit t=%ums\n", (unsigned)millis());
  bool ok = hwInit();
  if (ok) {
    deferred_ = false;
    autoRetryCount_ = 0;
  } else {
    deferred_ = true;
    lastAutoRetryMs_ = millis();
    if (autoRetryCount_ < NFC_AUTO_RETRY_MAX) autoRetryCount_ = autoRetryCount_ ? autoRetryCount_ : 1;
  }
  return ok;
}

void NfcReader::startListen(uint32_t listenMs) {
  listen_ = true;
  listenUntilMs_ = listenMs ? (millis() + listenMs) : 0;
  nextPollMs_ = millis();
  Serial.printf("[NFC] listen %s\n",
                listenMs ? "window" : "continuous");
}

void NfcReader::holdSclHigh() {
  int sda = sda_ >= 0 ? sda_ : PIN_NFC_SDA;
  int scl = scl_ >= 0 ? scl_ : PIN_NFC_SCL;
  releaseBus(sda, scl);  // 只松 Wire；SDA 保持上拉，只推 SCL
  pinMode(scl, OUTPUT);
  digitalWrite(scl, HIGH);
  Serial.printf("[NFC] SCL hold HIGH pin=%d (SDA 保持上拉)\n", scl);
}

void NfcReader::releaseScl() {
  int scl = scl_ >= 0 ? scl_ : PIN_NFC_SCL;
  int sda = sda_ >= 0 ? sda_ : PIN_NFC_SDA;
  forceIdlePullups(sda, scl);
  delay(5);
  Serial.printf("[NFC] release pullup SDA=%d SCL=%d\n", digitalRead(sda),
                digitalRead(scl));
}

bool NfcReader::poll(String& uid) {
  uint32_t now = millis();

  if (!ok_) {
    if (millisReached(now, nextPollMs_)) {
      maybeRecover();
      nextPollMs_ = now + 400;
    }
    return false;
  }

  if (!listen_) return false;
  if (listenUntilMs_ && millisReached(now, listenUntilMs_)) {
    listen_ = false;
    Serial.println("[NFC] listen window end");
  }
  if (!millisReached(now, nextPollMs_)) return false;

  // 上一次慢 ACK：先丢残留 RDY，再发 InList（避免交替 1.3s 脏 ACK）
  if (lastPollSlow_) {
    pn532Drain();
    lastPollSlow_ = false;
  }

  // 仅「连续空轮询够多」才刷 RF 场；绝不能 empty=1 就刷
  // （场 on+rewire 后立刻 InList 会打出 1.2s 慢 ACK，形成死循环）
  if (emptyPolls_ >= NFC_FIELD_REFRESH_POLLS) {
    Serial.printf("[NFC] 空轮询 %u → 刷新 RF field\n", (unsigned)emptyPolls_);
    pn532RfFieldOn(sda_, scl_);
    emptyPolls_ = 0;
    lastFieldMs_ = now;
    nextPollMs_ = millis() + 200;  // rewire 后多等一会再 InList
    return false;
  }

  uint8_t buf[16];
  uint8_t len = 0;
  uint32_t tPoll = millis();
  // 固定足够贴卡窗口：不再随 pollGap 缩到 80ms（跟踪期曾导致漏刷）
  uint16_t to = NFC_READ_TIMEOUT_MS;
  uint8_t ret = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &len, to);
  uint32_t gap = pollGapMs_ ? pollGapMs_ : NFC_POLL_GAP_BT_TRACK_MS;
  nextPollMs_ = millis() + gap;
  uint32_t cost = millis() - tPoll;

  if (!busIdle(sda_, scl_)) {
    Serial.println("[NFC] poll bus LOW → release + resync");
    releaseBus(sda_, scl_);
    recoverBusAndResync();
    lastPollSlow_ = true;
    slowAckStreak_ = 0;
    emptyPolls_ = 0;
    return false;
  }

  if (!ret || len < 4) {
    // 慢 ACK：rewire + drain，并 80ms 内立刻再试一次（卡可能还贴着）
    if (cost > 800) {
      Serial.printf("[NFC] poll ACK 慢 %ums → rewire streak=%u\n",
                    (unsigned)cost, (unsigned)(slowAckStreak_ + 1));
      nfcRewire(sda_, scl_);
      pn532Drain();
      lastPollSlow_ = true;
      if (slowAckStreak_ < 255) slowAckStreak_++;
      if (slowAckStreak_ >= NFC_SLOW_STREAK_RESYNC) {
        Serial.println("[NFC] 连续慢 ACK → resync");
        recoverBusAndResync();
        slowAckStreak_ = 0;
      }
      nextPollMs_ = millis() + NFC_SLOW_RETRY_MS;
      return false;
    }
    // 正常无卡：不要例行 drain（会刷 Error 263 并可能打乱总线）
    lastPollSlow_ = false;
    slowAckStreak_ = 0;
    if (emptyPolls_ < 100000) emptyPolls_++;
    static uint32_t lastQuietLog = 0;
    if (listen_ && millis() - lastQuietLog > 5000) {
      lastQuietLog = millis();
      Serial.printf("[NFC] poll 无卡 ret=%d len=%u cost=%ums SCL=%d empty=%u\n",
                    (int)ret, (unsigned)len, (unsigned)cost,
                    digitalRead(scl_), (unsigned)emptyPolls_);
    }
    return false;
  }

  failStreak_ = 0;
  lastOkMs_ = now;
  lastPollSlow_ = false;
  slowAckStreak_ = 0;
  emptyPolls_ = 0;
  lastFieldMs_ = now;

  uid = "";
  for (uint8_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) uid += "0";
    uid += String(buf[i], HEX);
  }
  uid.toUpperCase();

  if (uid == lastUid_ && (now - lastReadMs_) < NFC_COOLDOWN_MS) {
    return false;
  }
  lastUid_ = uid;
  lastReadMs_ = now;
  return true;
}

bool NfcReader::isAuthorized(const String& uid) const {
  if (authUid_.length() == 0) return false;
  return uid.equalsIgnoreCase(authUid_);
}

String NfcReader::debugLine() const {
  const char* st = ok_ ? "ok" : (deferred_ ? "defer" : "wait");
  return "nfc=" + String(st) + " fail=" + String(failStreak_) +
         " retry=" + String(autoRetryCount_) +
         " auth=" + (authUid_.length() ? authUid_ : String("-"));
}
