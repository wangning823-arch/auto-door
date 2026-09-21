#include "nfc_reader.h"
#include "config.h"
#include <Wire.h>
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

#define NFC_COOLDOWN_MS 1500
#define NFC_POLL_MIN_MS 350
#define NFC_RECOVER_GAP_MS 15000
#define NFC_INIT_DELAY_MS 8000
#define NFC_FAIL_BEFORE_RESYNC 3

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
  uint16_t oldTo = 200;
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
  Wire.setTimeOut(oldTo);
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
    bool retriesOk = nfc.setPassiveActivationRetries(0x01);
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
    bool ack = nfc.setPassiveActivationRetries(0x01);
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
  Serial.printf("[NFC] PN532 ready 0x%08X\n", ver);
  return true;
}

bool NfcReader::begin(int sda, int scl) {
  sda_ = sda;
  scl_ = scl;
  ok_ = false;
  deferred_ = true;
  listen_ = false;
  nextPollMs_ = millis() + 200;
  lastRecoverMs_ = millis() - NFC_RECOVER_GAP_MS;
  forceIdlePullups(sda_, scl_);
  Serial.println("[NFC] setup 跳过硬件初始化（仅 nfcinit），t=" +
                 String((unsigned)millis()) + "ms");
  return false;
}

void NfcReader::maybeRecover() {
  uint32_t now = millis();
  if (ok_) return;
  // 失败后只许手动 nfcinit，避免 Error263 风暴把 SCL 再锁死
  if (deferred_) return;
  if (now - lastRecoverMs_ < NFC_RECOVER_GAP_MS) {
    return;
  }
  lastRecoverMs_ = now;
  Serial.printf("[NFC] recover try fail=%u\n", failStreak_);
  if (!hwInit()) deferred_ = true;
}

bool NfcReader::forceInit() {
  lastRecoverMs_ = millis();
  Serial.printf("[NFC] forceInit t=%ums\n", (unsigned)millis());
  bool ok = hwInit();
  // 失败不许后台自动再撞（上次 nfcinit 后 Error263 风暴把 SCL 锁 0.04）
  deferred_ = !ok;
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
    if (now >= nextPollMs_) {
      maybeRecover();
      nextPollMs_ = now + 400;
    }
    return false;
  }

  if (!listen_) return false;
  if (listenUntilMs_ && now >= listenUntilMs_) {
    listen_ = false;
    Serial.println("[NFC] listen window end");
  }
  if (now < nextPollMs_) return false;

  // 不在 listen 里刷 RF：开场后场应保持；再发 RFConfiguration 会和 InList 抢 ACK

  uint8_t buf[16];
  uint8_t len = 0;
  uint32_t tPoll = millis();
  uint8_t ret = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &len, 250);
  nextPollMs_ = millis() + 300;
  uint32_t cost = millis() - tPoll;

  // 轮询超时/无卡都可能时钟拉伸；总线低必须先松手再收
  if (!busIdle(sda_, scl_)) {
    Serial.println("[NFC] poll bus LOW → release + resync");
    releaseBus(sda_, scl_);
    recoverBusAndResync();
    return false;
  }

  if (!ret || len < 4) {
    // ACK 超时（>800ms）说明总线/芯片脏：rewire 一次，别等 1.3s 死循环
    if (cost > 800) {
      Serial.printf("[NFC] poll ACK 慢 %ums → rewire\n", (unsigned)cost);
      nfcRewire(sda_, scl_);
      pn532Drain();
      nextPollMs_ = millis() + 400;
      return false;
    }
    static uint32_t lastQuietLog = 0;
    if (listen_ && millis() - lastQuietLog > 5000) {
      lastQuietLog = millis();
      Serial.printf("[NFC] poll 无卡 ret=%d len=%u cost=%ums SCL=%d\n",
                    (int)ret, (unsigned)len, (unsigned)cost,
                    digitalRead(scl_));
    }
    return false;
  }

  failStreak_ = 0;
  lastOkMs_ = now;

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
  return "nfc=" + String(ok_ ? "ok" : "fail") +
         " fail=" + String(failStreak_) +
         " auth=" + (authUid_.length() ? authUid_ : String("-"));
}
