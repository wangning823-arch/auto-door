#include "nfc_reader.h"
#include "config.h"
#include "log_ship.h"
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_PN532.h>
#include <driver/gpio.h>

// 16/17 引脚定义不变（SDA=16 SCL=17）。0xFF=不用库的 IRQ/RESET 脚。
static Adafruit_PN532 nfc((uint8_t)0xFF, (uint8_t)0xFF);
static NfcReader* s_nfcSelf = nullptr;

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

#define NFC_COOLDOWN_MS 800   // 同一张卡防连读；太短会把一次贴卡读成开+关两次 toggle
#define NFC_POLL_MIN_MS 350
#define NFC_RECOVER_GAP_MS 15000
#define NFC_INIT_DELAY_MS 8000
#define NFC_FAIL_BEFORE_RESYNC 3
// 上电自动 init：给 WiFi/BT 起完再碰 I2C，避免和启动抢总线
#define NFC_BOOT_INIT_DELAY_MS 5000
// 失败后慢速自动重试（防止 15s 级 Error263 风暴锁死 SCL）
// 达到 MAX 后仍每 30min 再试一次，避免夜间一次 I2C 毛刺就永久失联
#define NFC_AUTO_RETRY_GAP_MS (10UL * 60UL * 1000UL)
#define NFC_AUTO_RETRY_MAX 20
#define NFC_SLOW_KEEPALIVE_MS (30UL * 60UL * 1000UL)
// 未接模块时的在场探测：必须短超时，否则每次 NACK 等 1s 把 Web/NFC 轮询堵死
#define NFC_PROBE_TIMEOUT_MS 40
#define NFC_PROBE_RETRIES 3

// （实现见上方 i2cBusRecover：前向声明供 pn532WaitRdy 等使用）

static bool pn532SetRetries(uint8_t retries);

static bool busIdle(int sda, int scl) {
  forceIdlePullups(sda, scl);
  delay(2);
  return digitalRead(sda) && digitalRead(scl);
}

// SCL 被从机/半截传输按死时：Wire.end 还脚 + 推挽 9-clock。
// 关键：时钟必须是推挽 OUTPUT；开漏 HIGH 只是松手，从机仍可按住 SCL。
// 注意：ESP32 pinMode(OUTPUT) 会按输出寄存器（默认 0）驱动 → 必须先写 1 再改模式，
// 否则会先打出一个 SCL 低毛刺，把空闲 PN532 弄成一直拉 SCL。
static void i2cBusRecover(int sda, int scl) {
  Wire.end();
  delay(2);
  gpio_reset_pin((gpio_num_t)scl);
  gpio_reset_pin((gpio_num_t)sda);
  digitalWrite(scl, HIGH);  // 先设输出寄存器
  pinMode(scl, OUTPUT);     // 再使能输出，避免低毛刺
  delay(50);
  digitalWrite(sda, HIGH);
  pinMode(sda, OUTPUT);
  delay(2);
  int pushScl = digitalRead(scl);
  for (int round = 0; round < 5; round++) {
    for (int i = 0; i < 9; i++) {
      digitalWrite(scl, LOW);
      delayMicroseconds(80);
      digitalWrite(scl, HIGH);
      delayMicroseconds(80);
    }
    digitalWrite(sda, LOW);
    delayMicroseconds(80);
    digitalWrite(sda, HIGH);
    delayMicroseconds(80);
    if (digitalRead(sda) && digitalRead(scl)) break;
    delay(5);
  }
  // 推挽态读回：1=我们能驱动；0=硬短/对地
  int pushAfter = digitalRead(scl);
  forceIdlePullups(sda, scl);
  delay(5);
  Serial.printf("[NFC] bus recover pushScl=%d pushAfter=%d idleScl=%d\n",
                pushScl, pushAfter, digitalRead(scl));
}

// I2C 超时后必须松手：否则 ESP 外设/从机时钟拉伸会把 SCL 按在 0.04
// Wire.end() 不一定把脚从 I2C 矩阵断开；必须 gpio_reset_pin 才回到 GPIO 上拉
static void releaseBus(int sda, int scl) {
  Wire.end();
  if (scl >= 0) gpio_reset_pin((gpio_num_t)scl);
  if (sda >= 0) gpio_reset_pin((gpio_num_t)sda);
  forceIdlePullups(sda, scl);
  delay(2);
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);  // 禁止把 1000ms 超时泄漏到下一轮
}

// init 失败统一收尾：松手 + 计失败（禁止后台自动再撞，只允许手动 nfcinit）
static void failRelease(const char* why, int sda, int scl, uint16_t* streak) {
  Serial.printf("[NFC] FAIL %s SCL=%d → Wire.end\n", why, digitalRead(scl));
  releaseBus(sda, scl);
  if (*streak < 60000) (*streak)++;
}

// 在场探测：必须用「地址 ACK」，不能用读应答。
// PN532 空闲无数据时，读 0x24 会 NACK（协议如此），曾被误判为「未接模块」
// → absent_ + autoRetry 打满 → 永久不再 init（今早 NFC 失灵的根因）。
bool NfcReader::probePresent() {
  if (sda_ < 0) return false;
  releaseBus(sda_, scl_);
  if (!busIdle(sda_, scl_)) {
    i2cBusRecover(sda_, scl_);
    if (!busIdle(sda_, scl_)) return false;
  }
  Wire.begin(sda_, scl_, (uint32_t)100000);
  Wire.setTimeOut(NFC_PROBE_TIMEOUT_MS);
  forceIdlePullups(sda_, scl_);

  const uint8_t addr = 0x24;
  int hits = 0;
  for (int i = 0; i < NFC_PROBE_RETRIES; i++) {
    Wire.beginTransmission(addr);
    // endTransmission()==0 表示从机 ACK 了地址（芯片在；与是否有数据无关）
    if (Wire.endTransmission() == 0) hits++;
    delay(5);
  }
  Wire.setTimeOut(200);
  releaseBus(sda_, scl_);
  forceIdlePullups(sda_, scl_);
  return hits > 0;
}

// ===== 裸 PN532 I2C：绕开 Adafruit waitready（假时钟会把贴卡拖成 1.3s 超时）=====
static void pn532Drain();
static void nfcPulse9Clk() {
  // 只有总线被拉低才补时钟；空闲时乱打 9-clock 会把 PN532 弄乱，下条命令反而锁死 SCL
  if (digitalRead(PIN_NFC_SCL) && digitalRead(PIN_NFC_SDA)) return;
  Wire.end();
  digitalWrite(PIN_NFC_SCL, HIGH);
  pinMode(PIN_NFC_SCL, OUTPUT);
  digitalWrite(PIN_NFC_SDA, HIGH);
  pinMode(PIN_NFC_SDA, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(PIN_NFC_SCL, LOW);
    delayMicroseconds(80);
    digitalWrite(PIN_NFC_SCL, HIGH);
    delayMicroseconds(80);
  }
  digitalWrite(PIN_NFC_SDA, LOW);
  delayMicroseconds(80);
  digitalWrite(PIN_NFC_SDA, HIGH);
  delayMicroseconds(80);
  forceIdlePullups(PIN_NFC_SDA, PIN_NFC_SCL);
  delay(2);
}

static void pn532WriteCmd(const uint8_t* cmd, uint8_t cmdlen) {
  const uint8_t addr = PN532_I2C_ADDRESS;
  uint8_t packet[32];
  uint8_t len = (uint8_t)(cmdlen + 1);
  packet[0] = 0x00;
  packet[1] = 0x00;
  packet[2] = 0xFF;
  packet[3] = len;
  packet[4] = (uint8_t)(~len + 1);
  packet[5] = 0xD4;
  uint8_t sum = 0xD4;
  for (uint8_t i = 0; i < cmdlen; i++) {
    packet[6 + i] = cmd[i];
    sum = (uint8_t)(sum + cmd[i]);
  }
  packet[6 + cmdlen] = (uint8_t)(~sum + 1);
  packet[7 + cmdlen] = 0x00;
  Wire.beginTransmission(addr);
  Wire.write(packet, (uint8_t)(8 + cmdlen));
  Wire.endTransmission();
}

static void i2cBusRecover(int sda, int scl);

static bool pn532WaitRdy(uint32_t budgetMs) {
  uint32_t start = millis();
  Wire.setTimeOut(60);  // 与 isready 同：15ms 会掐断 InList 时钟拉伸
  bool ready = false;
  while ((millis() - start) < budgetMs) {
    uint8_t rdy = 0;
    uint8_t n = Wire.requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)1);
    if (n == 1 && Wire.available()) rdy = (uint8_t)Wire.read();
    if (rdy == PN532_I2C_READY) {
      ready = true;
      break;
    }
    delay(2);
  }
  // 不恢复 oldTo：init 期 1000ms 泄漏会让 isready NACK 卡成 cost≈1007ms
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
  return ready;
}

// 读一帧（先剥 RDY 字节）；budgetMs 内等 RDY
static int pn532ReadFrame(uint8_t* out, uint8_t maxn, uint32_t budgetMs) {
  if (!pn532WaitRdy(budgetMs)) return -1;
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
  uint8_t n = Wire.requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)(maxn + 1));
  if (n == 0) return -1;
  uint8_t seen = 0;
  if (Wire.available()) {
    Wire.read();  // RDY
    seen++;
  }
  int got = 0;
  while (Wire.available() && got < maxn && seen < n) {
    out[got++] = (uint8_t)Wire.read();
    seen++;
  }
  return got;
}

static bool pn532ReadAck(uint32_t budgetMs) {
  uint8_t buf[8] = {0};
  int n = pn532ReadFrame(buf, 6, budgetMs);
  if (n < 6) return false;
  // 00 00 FF 00 FF 00
  return buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFF && buf[3] == 0x00 &&
         buf[4] == 0xFF && buf[5] == 0x00;
}

// InList 是否仍在芯片里搜卡（retries=0xFF 时超时后芯片继续搜，禁止叠发新 InList）
static bool s_inlistOpen = false;
static uint32_t s_inlistOpenAt = 0;

// 等当前 InList 的出卡帧。s_inlistOpen 时只读不写，保护手机 HCE ATR。
// 返回 1=卡 0=本轮窗口未出卡（芯片可能仍在搜） -1=帧/总线错（才允许重发）
static int pn532InListRaw(uint8_t* uid, uint8_t* uidLen) {
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
  if (!s_inlistOpen) {
    uint8_t cmd[3] = {0x4A, 0x01, 0x04};  // InList, 1 tg, ISO14443A
    pn532WriteCmd(cmd, 3);
    // 写出去就算片上可能已开搜：ACK 失败也不许立刻叠发（芯片忙会 NACK→cost≈1s）
    s_inlistOpen = true;
    s_inlistOpenAt = millis();
    if (!pn532ReadAck(NFC_INLIST_ACK_MS)) {
      nfcPulse9Clk();
      return -1;
    }
  } else if (millis() - s_inlistOpenAt > NFC_INLIST_STUCK_MS) {
    // 粘滞过久仍无 ready：打断片上 InList，下一轮重发
    Serial.println("[NFC] inlist stuck → abort + resend");
    pn532SetRetries(0x01);
    pn532Drain();
    s_inlistOpen = false;
    return -1;
  }

  uint8_t resp[32] = {0};
  int n = pn532ReadFrame(resp, 28, NFC_INLIST_WAIT_MS);
  if (n < 8) {
    // 超时：芯片可能仍在寻卡。禁止 drain/重发（会重置手机 ATR → 弹窗无 UID）
    if (!digitalRead(PIN_NFC_SCL) || !digitalRead(PIN_NFC_SDA)) {
      // 先 9-clock，仍死才 full recover（recover 毛刺更容易锁死）
      nfcPulse9Clk();
      if (!digitalRead(PIN_NFC_SCL) || !digitalRead(PIN_NFC_SDA))
        i2cBusRecover(PIN_NFC_SDA, PIN_NFC_SCL);
      s_inlistOpen = false;
      return -1;
    }
    return 0;
  }
  // 收到完整帧 = 片上 InList 已结束（出卡或 0 tags），下一轮才可重发
  s_inlistOpen = false;
  // PN532 帧：00 00 FF LEN LCS TFI ... | 扩展 00 00 FF FF LENm LENl LCS TFI ...
  // 以前写死 p=5，扩展帧/错位会当成「非 D5」→ 永远无 UID、不发 RF
  int p = -1;
  if (n >= 5 && resp[0] == 0x00 && resp[1] == 0x00 && resp[2] == 0xFF) {
    if (resp[3] != 0xFF) {
      p = 5;  // 短帧 TFI
    } else if (n >= 8) {
      p = 7;  // 扩展帧 TFI
    }
  }
  if (p < 0 || p >= n) {
    // 兜底：直接找 D5 4B（InListPassiveTarget 应答）
    for (int i = 0; i + 1 < n && i < 16; i++) {
      if (resp[i] == 0xD5 && resp[i + 1] == 0x4B) {
        p = i;
        break;
      }
    }
  }
  if (p < 0 || p + 1 >= n || resp[p] != 0xD5) {
    Serial.printf("[NFC] inlist parse n=%d", n);
    for (int i = 0; i < n && i < 16; i++) Serial.printf(" %02X", resp[i]);
    Serial.println();
    return -1;
  }
  p++;  // D5
  if (p >= n || resp[p] != 0x4B) return -1;
  p++;  // 4B
  if (p >= n) return -1;
  uint8_t nb = resp[p++];
  if (nb == 0) return 0;
  if (p + 3 >= n) return -1;
  p++;     // Tg
  p += 2;  // ATQA
  p++;     // SAK
  if (p >= n) return -1;
  uint8_t idLen = resp[p++];
  if (idLen < 4 || idLen > 7 || p + idLen > n) {
    Serial.printf("[NFC] inlist uid parse idLen=%u n=%d\n", (unsigned)idLen, n);
    return -1;
  }
  for (uint8_t i = 0; i < idLen; i++) uid[i] = resp[p + i];
  *uidLen = idLen;
  return 1;
}

// 裸发命令并吃掉响应：setRetries/SAMConfig 不再走 Adafruit waitready
static bool pn532Xfer(const uint8_t* cmd, uint8_t cmdlen, uint32_t waitMs) {
  pn532WriteCmd(cmd, cmdlen);
  if (!pn532ReadAck(200)) return false;
  uint8_t resp[24] = {0};
  int n = pn532ReadFrame(resp, 20, waitMs);
  return n >= 3 && resp[0] == 0x00 && resp[1] == 0x00 && resp[2] == 0xFF;
}

// RFConfiguration item5：MxRtyPassiveActivation = retries
static bool pn532SetRetries(uint8_t retries) {
  uint8_t cmd[5] = {0x32, 0x05, 0xFF, 0x01, retries};
  return pn532Xfer(cmd, 5, 200);
}

// 上电/掉线后 PN532 可能睡死：先发 dummy 地址字节唤醒
static void pn532Wakeup() {
  Wire.beginTransmission(PN532_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
  delay(2);
}

// SAMConfig：normal mode, 不用 IRQ（未接 IRQ 时 useIRQ=1 会卡命令）
static bool pn532SamConfig() {
  pn532Wakeup();
  uint8_t cmd[4] = {0x14, 0x01, 0x14, 0x00};
  return pn532Xfer(cmd, 4, 300);
}

// GetFirmwareVersion
static uint32_t pn532GetFwVer() {
  uint8_t cmd[1] = {0x02};
  pn532WriteCmd(cmd, 1);
  if (!pn532ReadAck(100)) return 0;
  uint8_t resp[24] = {0};
  int n = pn532ReadFrame(resp, 20, 300);
  if (n < 11) return 0;
  int p = 0;
  if (resp[0] == 0x00 && resp[1] == 0x00 && resp[2] == 0xFF) p = 5;
  if (p + 5 >= n || resp[p] != 0xD5 || resp[p + 1] != 0x03) return 0;
  uint32_t ver = ((uint32_t)resp[p + 2] << 24) | ((uint32_t)resp[p + 3] << 16) |
                 ((uint32_t)resp[p + 4] << 8) | (uint32_t)resp[p + 5];
  return ver;
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
  Wire.setTimeOut(oldTo ? oldTo : NFC_WIRE_TIMEOUT_MS);
}

// 重开 I2C：RF/长超时后外设状态脏，残留 RDY 会让下一条 ACK 等到 1.3s
static void nfcRewire(int sda, int scl) {
  releaseBus(sda, scl);
  delay(25);
  Wire.begin(sda, scl, (uint32_t)100000);
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
  forceIdlePullups(sda, scl);
  delay(15);
}

// 持续开 RF 场：必须走裸命令；库 sendCommandCheckAck 的 waitready 会把总线弄脏
static bool pn532RfFieldOn(int sda, int scl) {
  (void)sda;
  (void)scl;
  pn532Drain();
  delay(10);
  uint8_t rfOn[3] = {0x32, 0x01, 0x01};
  bool ok = pn532Xfer(rfOn, 3, 200);
  Serial.printf("[NFC] RF field raw ack=%d\n", (int)ok);
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
  return ok;
}

// 读卡连续失败后的总线复活：不整颗 nfc.begin（避免和成功路径打架）
bool NfcReader::recoverBusAndResync() {
  uint32_t now = millis();
  if (now - lastResyncMs_ < 3000) return false;
  lastResyncMs_ = now;
  Serial.println("[NFC] resync bus + SAMConfig");

  releaseBus(sda_, scl_);
  // 只有总线不空闲才 recover，避免毛刺弄死空闲 PN532
  if (!busIdle(sda_, scl_)) {
    i2cBusRecover(sda_, scl_);
    if (!busIdle(sda_, scl_)) {
      failRelease("resync idle", sda_, scl_, &failStreak_);
      ok_ = false;
      deferred_ = true;
      return false;
    }
  }
  Wire.begin(sda_, scl_, (uint32_t)100000);
  // init 可以宽一点，结束前必须收回短超时，否则 isready NACK 会拖成 1s 级慢 ACK
  Wire.setTimeOut(1000);
  delay(50);

  if (!pn532SamConfig()) {
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
    bool retriesOk = pn532SetRetries(NFC_INLIST_RETRIES);
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
  s_inlistOpen = false;
  lastFieldMs_ = millis();
  Wire.setTimeOut(NFC_WIRE_TIMEOUT_MS);
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

  // 未接芯片：先短超时探测，避免 1000ms×N 次把 HTTP/主循环堵死
  if (!probePresent()) {
    absent_ = true;
    ok_ = false;
    deferred_ = true;
    // 不再打满 autoRetry：允许慢速保活重试，避免一次毛刺后永久失联
    bootInitDone_ = true;
    if (failStreak_ < 60000) failStreak_++;
    logShipf("[NFC] probe no ACK → defer (absent?) SCL=%d", digitalRead(scl_));
    releaseBus(sda_, scl_);
    forceIdlePullups(sda_, scl_);
    return false;
  }
  absent_ = false;

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
  logShipf("[NFC] PN532 ready 0x%08X", ver);
  return true;
}

bool NfcReader::begin(int sda, int scl, int irqPin) {
  sda_ = sda;
  scl_ = scl;
  irq_ = irqPin;
  ok_ = false;
  // 不再上电即永久 deferred：排程一次自动 init，断电重启后刷卡可自恢复
  deferred_ = false;
  absent_ = false;
  bootInitDone_ = false;
  bootInitAt_ = millis() + NFC_BOOT_INIT_DELAY_MS;
  lastAutoRetryMs_ = 0;
  autoRetryCount_ = 0;
  listen_ = false;  // hwInit 成功后自动 listen
  bootPostpones_ = 0;
  nextPollMs_ = millis() + 200;
  lastRecoverMs_ = millis() - NFC_RECOVER_GAP_MS;
  forceIdlePullups(sda_, scl_);

  if (!busMux_) busMux_ = xSemaphoreCreateMutex();
  if (!cardQ_) cardQ_ = xQueueCreate(4, 32);  // uid 最长 28+1
  irqWired_ = detectIrqWired();
  if (irqWired_ && irq_ >= 0) {
    pinMode(irq_, INPUT);  // 模块侧上拉；FALLING=有事件
    attachInterrupt(digitalPinToInterrupt(irq_), []() {
      // 仅唤醒任务；I2C 在任务里做
      if (s_nfcSelf && s_nfcSelf->task_) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_nfcSelf->task_, &hp);
        if (hp == pdTRUE) portYIELD_FROM_ISR();
      }
    }, FALLING);
    Serial.printf("[NFC] IRQ detected GPIO%d → 事件驱动\n", irq_);
  } else {
    Serial.printf("[NFC] IRQ GPIO%d not wired → FreeRTOS 任务轮询\n",
                  irq_ >= 0 ? irq_ : -1);
  }

  if (!task_) {
    s_nfcSelf = this;
    xTaskCreatePinnedToCore(taskTrampoline, "nfc", 6144, this, 1, &task_, 1);
    Serial.println("[NFC] async task started (loop 不再阻塞读卡)");
  }

  Serial.printf("[NFC] setup：硬件初始化已排程，约 %ums 后自动 nfcinit\n",
                (unsigned)NFC_BOOT_INIT_DELAY_MS);
  return false;
}

// 探测 IRQ 是否外接：模块 IRQ 空闲为高（板上拉）。
// 未接线时内部下拉应读到 LOW；被外部拉高则读到 HIGH。
bool NfcReader::detectIrqWired() {
  if (irq_ < 0 || irq_ > 39) return false;
  pinMode(irq_, INPUT_PULLDOWN);
  delayMicroseconds(30);
  int a = digitalRead(irq_);
  delayMicroseconds(30);
  int b = digitalRead(irq_);
  // 再用上拉复核：真正接模块时两种电阻下都应稳定为高
  pinMode(irq_, INPUT_PULLUP);
  delayMicroseconds(30);
  int c = digitalRead(irq_);
  pinMode(irq_, INPUT_PULLDOWN);
  return (a == HIGH && b == HIGH && c == HIGH);
}

bool NfcReader::lockBus(uint32_t timeoutMs) {
  if (!busMux_) return true;
  return xSemaphoreTake(busMux_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void NfcReader::unlockBus() {
  if (busMux_) xSemaphoreGive(busMux_);
}

void NfcReader::stopForOta() {
  suspended_ = true;
  listen_ = false;
  if (lockBus(300)) {
    // 1) 把片上寻卡收到短重试，让进行中的 InList 尽快自己结束
    pn532SetRetries(0x01);
    // 2) 等一条 InList 窗口走完（有限 retries 会在数百 ms 内回 0 tags/出卡）
    uint32_t t0 = millis();
    while (s_inlistOpen && millis() - t0 < 800) {
      uint8_t tmp[32] = {0};
      int n = pn532ReadFrame(tmp, 28, 100);
      if (n >= 8) break;
      if (!digitalRead(PIN_NFC_SCL) || !digitalRead(PIN_NFC_SDA)) break;
    }
    pn532Drain();
    s_inlistOpen = false;
    releaseBus(sda_, scl_);
    unlockBus();
  }
  Serial.println("[NFC] stopForOta: InList aborted, bus released");
}

void NfcReader::pushCard(const String& uid) {
  if (!cardQ_) return;
  char buf[32] = {0};
  uid.toCharArray(buf, sizeof(buf));
  xQueueSend(cardQ_, buf, 0);
}

void NfcReader::service() {
  if (!cardQ_ || !onCard_) return;
  char buf[32];
  while (xQueueReceive(cardQ_, buf, 0) == pdTRUE) {
    String uid(buf);
    if (uid.length()) onCard_(uid);
  }
}

void NfcReader::taskTrampoline(void* arg) {
  static_cast<NfcReader*>(arg)->taskLoop();
}

void NfcReader::taskLoop() {
  for (;;) {
    if (suspended_) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (irqWired_ && ok_ && listen_) {
      // 无卡时睡死等 IRQ；超时只为 maybeRecover/场刷新
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pollGapMs_ ? pollGapMs_ : 350));
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (suspended_) continue;
    if (!lockBus(200)) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    String uid;
    bool got = poll(uid);  // 内部含 maybeRecover / gap
    unlockBus();
    if (got) pushCard(uid);
  }
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
    // STA 网页未起来时再等一会：无芯片探测也别和 DHCP/HTTP 抢 loop
    // （有 STA 配置且未连上时最多等到 15s；无 STA 则按原 5s）
    // 注：探测本身仅 ~120ms，这里主要让 HTTP 先出日志/可访问
    bootInitDone_ = true;
    Serial.printf("[NFC] 上电自动 init t=%ums SCL=%d\n", (unsigned)now,
                  digitalRead(scl_ >= 0 ? scl_ : PIN_NFC_SCL));
    if (hwInit()) {
      logShipf("[NFC] boot auto-init OK");
      return;
    }
    lastAutoRetryMs_ = now;
    autoRetryCount_ = 1;
    logShipf("[NFC] boot auto-init fail absent=%d → slow retry", (int)absent_);
    return;
  }

  // 2) deferred：慢速重试。超过 MAX 仍按 30min 保活再试，禁止永久放弃
  if (deferred_) {
    // 总线被按住时不要 3s 连撞（i2cBusRecover/Wire.begin 会再占 I2C 矩阵）
    // 前几次快速重试：OTA 重启后 PN532 可能 1-2s 内还忙，30s 太久
    uint32_t gap = (failStreak_ > 0 && !digitalRead(scl_ >= 0 ? scl_ : PIN_NFC_SCL))
                       ? 30000UL
                       : ((autoRetryCount_ < 5) ? 3000UL : NFC_AUTO_RETRY_GAP_MS);
    if (autoRetryCount_ >= NFC_AUTO_RETRY_MAX) gap = NFC_SLOW_KEEPALIVE_MS;
    if (!millisReached(now, lastAutoRetryMs_ + gap)) return;
    lastAutoRetryMs_ = now;
    if (autoRetryCount_ < 255) autoRetryCount_++;
    logShipf("[NFC] auto-retry %u t=%ums fail=%u absent=%d", autoRetryCount_,
             (unsigned)now, failStreak_, (int)absent_);
    if (hwInit()) {
      autoRetryCount_ = 0;
      logShipf("[NFC] auto-retry OK");
    }
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
  Serial.printf("[NFC] forceInit t=%ums absent=%d\n", (unsigned)millis(),
                (int)absent_);
  // 手动 nfcinit：清 absent/重试计数，允许再短探 + 完整 init
  absent_ = false;
  autoRetryCount_ = 0;
  if (!lockBus(2000)) {
    logShipf("[NFC] forceInit bus busy");
    deferred_ = true;
    lastAutoRetryMs_ = millis();
    return false;
  }
  bool ok = hwInit();
  unlockBus();
  logShipf("[NFC] forceInit t=%ums absent=%d", (unsigned)millis(), (int)absent_);
  if (ok) {
    deferred_ = false;
    autoRetryCount_ = 0;
    logShipf("[NFC] forceInit OK ver ready");
  } else {
    deferred_ = true;
    lastAutoRetryMs_ = millis();
    if (autoRetryCount_ < 1) autoRetryCount_ = 1;
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
  digitalWrite(scl, HIGH);
  pinMode(scl, OUTPUT);
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

  // 总线被按住就先复活：ok_ 为真时若不处理，会反复 InList 把芯片锁死到只能断电
  if (!digitalRead(sda_) || !digitalRead(scl_)) {
    Serial.printf("[NFC] poll bus stuck SDA=%d SCL=%d → recover\n",
                  digitalRead(sda_), digitalRead(scl_));
    nfcRewire(sda_, scl_);
    if (!busIdle(sda_, scl_)) {
      i2cBusRecover(sda_, scl_);
      if (!busIdle(sda_, scl_)) {
        ok_ = false;
        deferred_ = true;
        lastAutoRetryMs_ = now;
        logShipf("[NFC] poll bus hard-stuck → deferred");
        return false;
      }
    }
    s_inlistOpen = false;
    nextPollMs_ = now + 200;
    return false;
  }

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
  // 用 raw InList：片上搜卡未结束时不叠发新命令（Adafruit 路径 200ms 超时后会连撞）
  int got = pn532InListRaw(buf, &len);
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

  if (got <= 0) {
    // got=-1：总线/帧错（raw 路径已尝试 9-clock）；got=0：本轮未出卡（芯片可能仍在搜）
    if (got < 0) {
      lastPollSlow_ = true;
      if (slowAckStreak_ < 255) slowAckStreak_++;
      if (slowAckStreak_ >= NFC_SLOW_STREAK_RESYNC) {
        Serial.println("[NFC] 连续 InList 错 → resync");
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
      Serial.printf("[NFC] poll 无卡 cost=%ums SCL=%d empty=%u\n",
                    (unsigned)cost, digitalRead(scl_), (unsigned)emptyPolls_);
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
  const char* st = ok_ ? "ok" : (absent_ ? "nochip" : (deferred_ ? "defer" : "wait"));
  return "nfc=" + String(st) + " fail=" + String(failStreak_) +
         " retry=" + String(autoRetryCount_) +
         " irq=" + String(irqWired_ ? 1 : 0) +
         " auth=" + (authUid_.length() ? authUid_ : String("-"));
}
