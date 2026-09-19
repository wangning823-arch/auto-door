#include "rf_capture.h"

static volatile uint16_t rfPulseBuf[RF_CAPTURE_MAX_PULSES];
static volatile uint32_t rfLastChangeUs = 0;
static volatile uint16_t rfIdx = 0;
static volatile bool rfCapturing = false;
// 真·数据脉冲（≥80µs 且 <5ms）：有足够多才允许因帧间隙提前结束
static volatile uint16_t rfDataN = 0;
void (*RfCapture::idleHook_)() = nullptr;
void (*RfCapture::startHook_)() = nullptr;
volatile bool RfCapture::stopReq_ = false;

static inline bool rfIsDataPulse(uint32_t dur) {
  return dur >= 80 && dur < 5000;
}

static void IRAM_ATTR rfIsr() {
  if (!rfCapturing) return;
  uint32_t now = micros();
  uint32_t dur = now - rfLastChangeUs;
  rfLastChangeUs = now;

  if (rfIdx == 0 && dur >= RF_CAPTURE_GAP_US) {
    return;
  }
  if (dur >= RF_CAPTURE_GAP_US) {
    // 只有已经收到像样的数据帧才靠间隙收尾；纯底噪继续听满超时
    if (rfDataN >= 15 && rfIdx > 10) rfCapturing = false;
    return;
  }
  if (rfIdx < RF_CAPTURE_MAX_PULSES) {
    uint16_t store = (dur > 65535) ? 65535 : (uint16_t)dur;
    rfPulseBuf[rfIdx++] = store;
    if (rfIsDataPulse(dur)) rfDataN++;
  }
}

void RfCapture::begin(int rxPin, int txPin) {
  rxPin_ = rxPin;
  txPin_ = txPin;
  pinMode(rxPin_, INPUT);
  pinMode(txPin_, OUTPUT);
  digitalWrite(txPin_, LOW);
  for (int i = 0; i < RF_KEY_COUNT; i++) keyLen_[i] = 0;
}

bool RfCapture::capture(uint32_t timeoutMs) {
  if (rxPin_ < 0) return false;

  if (count_ > 0) {
    memcpy(lastPulses_, pulses_, count_ * sizeof(uint16_t));
    lastCount_ = count_;
    hasLast_ = true;
  }

  rfIdx = 0;
  rfDataN = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  count_ = 0;

  Serial.printf("[RF] 等待信号... (RX=GPIO%d, 超时 %ums)\n", rxPin_, timeoutMs);
  Serial.println("[RF] 请短按遥控器（5~10cm 对准天线）...");

  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);

  uint32_t start = millis();
  uint16_t lastPrinted = 0;
  uint32_t lastHookMs = 0;
  uint32_t lastProgMs = 0;
  bool sawData = false;
  const uint32_t MIN_LISTEN_MS = 3500;
  while ((millis() - start) < timeoutMs) {
    if (!rfCapturing) break;
    if (idleHook_ && (millis() - lastHookMs) >= 20) {
      lastHookMs = millis();
      idleHook_();
    }
    uint16_t n = rfIdx;
    uint16_t d = rfDataN;
    if (d >= 15 && !sawData) {
      sawData = true;
      Serial.printf("[RF] 检测到数据脉冲 %u（继续听满 %ums 以便再按遥控）...\n",
                    d, MIN_LISTEN_MS);
    }
    if (n >= 10 && lastPrinted == 0) {
      Serial.printf("[RF] 已收到 %u 脉冲...\n", n);
      lastPrinted = n;
    }
    // 每秒进度：GUI/串口都能看到「还在抓」和边沿是否增长
    if ((millis() - lastProgMs) >= 1000) {
      lastProgMs = millis();
      uint32_t elapsed = (millis() - start) / 1000;
      Serial.printf("[RF] 听中 t=%lus 总脉冲=%u 数据=%u%s\n",
                    (unsigned long)elapsed, n, d,
                    sawData ? " [有数据]" : "");
    }
    if (sawData && n >= 10 && rfLastChangeUs > 0 &&
        (millis() - start) >= MIN_LISTEN_MS) {
      if ((micros() - rfLastChangeUs) > 25000) {
        rfCapturing = false;
        break;
      }
    }
    if (n == 0 && (millis() - start) > 2000 && (millis() - start) < 2200) {
      Serial.println("[RF] 还没检测到边沿，检查天线/距离/频率...");
    }
    delay(1);
  }
  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(rxPin_));
  delay(5);

  count_ = rfIdx;
  for (uint16_t i = 0; i < count_; i++) pulses_[i] = rfPulseBuf[i];
  uint16_t dataN = rfDataN;

  if (dataN < 15) {
    Serial.printf("[RF] 抓包失败：仅噪声（数据脉冲 %u，总 %u）\n", dataN, count_);
    Serial.println("[RF] 不是有效遥控码；请靠近天线/检查 RX 天线与 5V 后再抓");
    count_ = 0;
    Serial.println("[RF] RFCAP_END");
    return false;
  }
  if (count_ < 10) {
    Serial.printf("[RF] 抓包失败：仅 %u 个脉冲\n", count_);
    Serial.println("[RF] RFCAP_END");
    return false;
  }

  Serial.printf("[RF] 抓包成功：%u 个脉冲（数据脉冲 %u）\n", count_, dataN);
  Serial.print("[RF] pulses:");
  for (uint16_t i = 0; i < count_; i++) {
    Serial.print(' ');
    Serial.print(pulses_[i]);
    if (i + 1 < count_) Serial.print(',');
  }
  Serial.println();

  if (hasLast_) {
    Serial.println("[RF] ---- 与上次对比 ----");
    compareWithLast();
  } else {
    Serial.println("[RF] 第一次抓包完成，再执行 rfcap 抓第二次对比");
  }
  Serial.println("[RF] RFCAP_END");
  return true;
}

// 连续抓包：收到有效帧就输出并清空缓冲继续听，直到 rfstop
bool RfCapture::captureContinuous() {
  if (rxPin_ < 0) return false;

  stopReq_ = false;
  rfIdx = 0;
  rfDataN = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  count_ = 0;

  Serial.println("[RF] RFCAP_OK 连续抓包模式（点停止或发 rfstop 结束）");
  Serial.printf("[RF] 等待信号... (RX=GPIO%d, 连续)\n", rxPin_);
  Serial.println("[RF] 请短按遥控器（5~10cm 对准天线）...");

  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);

  uint32_t start = millis();
  uint32_t lastHookMs = 0;
  uint32_t lastProgMs = 0;
  uint32_t frameStartMs = millis();
  uint16_t frames = 0;
  String line;
  uint16_t lastTotal = 0;

  while (!stopReq_) {
    // 在阻塞循环里也能收到 rfstop（否则串口命令进不来）
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        line.trim();
        if (line == "rfstop") stopReq_ = true;
        line = "";
      } else if (line.length() < 32) {
        line += c;
      }
    }

    if (idleHook_ && (millis() - lastHookMs) >= 20) {
      lastHookMs = millis();
      idleHook_();
    }

    uint16_t n = rfIdx;
    uint16_t d = rfDataN;

    // 有效帧：数据够多 + 25ms 静默 → 打印本帧，清空继续听
    if (d >= 15 && n >= 10 && rfLastChangeUs > 0 &&
        (micros() - rfLastChangeUs) > 25000) {
      rfCapturing = false;
      delayMicroseconds(50);

      count_ = n;
      for (uint16_t i = 0; i < n && i < RF_CAPTURE_MAX_PULSES; i++)
        pulses_[i] = rfPulseBuf[i];

      frames++;
      Serial.printf("[RF] 抓包成功：%u 个脉冲（数据脉冲 %u）帧#%u\n", n, d, frames);
      Serial.print("[RF] pulses:");
      for (uint16_t i = 0; i < n; i++) {
        Serial.print(' ');
        Serial.print(pulses_[i]);
        if (i + 1 < n) Serial.print(',');
      }
      Serial.println();
      if (hasLast_ && lastCount_ > 0) {
        Serial.println("[RF] ---- 与上次对比 ----");
        compareWithLast();
      }
      // 当前帧存为下次的「上次」
      memcpy(lastPulses_, pulses_, n * sizeof(uint16_t));
      lastCount_ = n;
      hasLast_ = true;
      Serial.println("[RF] RFCAP_FRAME 本帧结束，继续监听...");
      Serial.println("[RF] 请短按遥控器（连续抓包进行中）...");

      // 清空缓冲，继续下一轮
      rfIdx = 0;
      rfDataN = 0;
      rfLastChangeUs = micros();
      lastTotal = 0;
      frameStartMs = millis();
      rfCapturing = true;
      continue;
    }

    if ((millis() - lastProgMs) >= 1000) {
      lastProgMs = millis();
      uint32_t elapsed = (millis() - frameStartMs) / 1000;
      uint32_t edgeAgeMs = (micros() - rfLastChangeUs) / 1000;
      int pinLv = digitalRead(rxPin_);
      if (n != lastTotal || d > 0) {
        Serial.printf("[RF] 听中 t=%lus 总脉冲=%u 数据=%u 帧数=%u 边沿静默=%lums pin=%d%s\n",
                      (unsigned long)elapsed, n, d, frames,
                      (unsigned long)edgeAgeMs, pinLv,
                      d >= 15 ? " [有数据]" : "");
        lastTotal = n;
      } else {
        Serial.printf("[RF] 听中 t=%lus （等待遥控）边沿静默=%lums pin=%d 帧数=%u\n",
                      (unsigned long)elapsed, (unsigned long)edgeAgeMs, pinLv, frames);
      }
      if (edgeAgeMs > 3000) {
        Serial.println("[RF] 警告: RX 超过 3s 无边沿 → 查模块 5V/GND/DATA(GPIO13)/天线；或 DATA 电平卡死");
      }
    }

    // 缓冲快满：先交出去再继续
    if (rfIdx >= RF_CAPTURE_MAX_PULSES - 8) {
      rfCapturing = false;
      delayMicroseconds(50);
      count_ = rfIdx;
      for (uint16_t i = 0; i < count_; i++) pulses_[i] = rfPulseBuf[i];
      Serial.printf("[RF] 缓冲将满，吐出 %u 脉冲（数据 %u）继续...\n",
                    count_, rfDataN);
      Serial.print("[RF] pulses:");
      for (uint16_t i = 0; i < count_; i++) {
        Serial.print(' ');
        Serial.print(pulses_[i]);
        if (i + 1 < count_) Serial.print(',');
      }
      Serial.println();
      Serial.println("[RF] RFCAP_FRAME");
      rfIdx = 0;
      rfDataN = 0;
      rfLastChangeUs = micros();
      rfCapturing = true;
      continue;
    }

    delay(1);
  }

  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(rxPin_));
  delay(5);
  count_ = rfIdx;
  for (uint16_t i = 0; i < count_; i++) pulses_[i] = rfPulseBuf[i];

  Serial.printf("[RF] 连续抓包结束，共 %u 帧\n", frames);
  Serial.println("[RF] RFCAP_END");
  stopReq_ = false;
  return frames > 0;
}

static uint16_t detectFrameLen(const uint16_t* p, uint16_t n) {
  if (n < 32) return n;
  uint16_t bestL = 0, bestScore = 0;
  const uint16_t minL = 16;
  const uint16_t maxL = (n / 2 > 400) ? 400 : (n / 2);
  for (uint16_t L = minL; L <= maxL; L++) {
    uint16_t score = 0, cmp = 0;
    uint16_t check = n - L;
    if (check > L * 2) check = L * 2;
    for (uint16_t i = 0; i < check; i++) {
      uint16_t a = p[i], b = p[i + L];
      uint16_t mx = a > b ? a : b;
      uint16_t d = a > b ? a - b : b - a;
      if (d <= (mx / 4 > 80 ? mx / 4 : 80)) score++;
      cmp++;
    }
    if (cmp >= 16 && score * 10 >= cmp * 8) {
      if (score > bestScore || (score == bestScore && L > bestL)) {
        bestScore = score;
        bestL = L;
      }
    }
  }
  return bestL ? bestL : n;
}

bool RfCapture::compareWithLast() {
  if (!hasLast_ || lastCount_ == 0 || count_ == 0) {
    Serial.println("[RF] 无对比数据");
    return false;
  }
  Serial.printf("[RF] 上次 %u 脉冲, 本次 %u 脉冲\n", lastCount_, count_);

  uint16_t la = detectFrameLen(lastPulses_, lastCount_);
  uint16_t lb = detectFrameLen(pulses_, count_);
  uint16_t frameLen = (la < lb) ? la : lb;
  if (frameLen > 240) frameLen = 240;
  Serial.printf("[RF] 帧长估计: %u / %u\n", la, lb);

  uint16_t mismatches = 0;
  for (uint16_t i = 0; i < frameLen; i++) {
    uint16_t a = pulses_[i], b = lastPulses_[i];
    uint16_t diff = (a > b) ? (a - b) : (b - a);
    uint16_t mx = (a > b) ? a : b;
    uint16_t tol = mx / 4;
    if (tol < 80) tol = 80;
    if (diff > tol) {
      mismatches++;
      if (mismatches <= 5) Serial.printf("[RF] 差异 #%u: %u vs %u\n", i, a, b);
    }
  }
  if (mismatches == 0) {
    Serial.println("[RF] ========== 码相同 ==========");
    Serial.println("[RF] 结论：固定码，可直接克隆回放");
    return true;
  }
  Serial.printf("[RF] 单帧内 %u 处脉冲不同 (共比较 %u)\n", mismatches, frameLen);
  Serial.println("[RF] ========== 码不同 ==========");
  Serial.println("[RF] 结论：可能是滚码，需要解码算法");
  return false;
}

bool RfCapture::extractOneFrame(uint16_t* out, uint16_t* outN, uint16_t maxN) const {
  if (count_ == 0 || maxN == 0) return false;

  // 按帧间隔分段，取最长段（首段可能被噪声切碎）
  uint16_t bestN = 0;
  uint16_t bestOff = 0;
  uint16_t curN = 0;
  uint16_t curOff = 0;
  for (uint16_t i = 0; i < count_; i++) {
    uint16_t v = pulses_[i];
    if (v >= RF_INTER_FRAME_MIN_US) {
      if (curN > bestN) {
        bestN = curN;
        bestOff = curOff;
      }
      curN = 0;
      curOff = i + 1;
      continue;
    }
    if (curN == 0) curOff = i;
    if (curN < maxN) curN++;
  }
  if (curN > bestN) {
    bestN = curN;
    bestOff = curOff;
  }

  if (bestN < 10) {
    // 容错：全序列当一帧（去掉开头长间隔）
    uint16_t n = 0;
    for (uint16_t i = 0; i < count_ && n < maxN; i++) {
      uint16_t v = pulses_[i];
      if (v >= RF_INTER_FRAME_MIN_US) {
        if (n > 0) break;
        continue;
      }
      out[n++] = v;
    }
    *outN = n;
    return n >= 10;
  }

  uint16_t n = 0;
  for (uint16_t i = bestOff; i < count_ && n < maxN && n < bestN; i++) {
    uint16_t v = pulses_[i];
    if (v >= RF_INTER_FRAME_MIN_US) break;
    out[n++] = v;
  }
  *outN = n;
  Serial.printf("[RF] 提帧: 最长段 off=%u len=%u\n", bestOff, n);
  return n >= 10;
}

static bool csvFromPulses(const uint16_t* p, uint16_t n, char* buf, size_t bufLen) {
  size_t off = 0;
  for (uint16_t i = 0; i < n; i++) {
    int w = snprintf(buf + off, bufLen - off, i ? ",%u" : "%u", p[i]);
    if (w < 0 || (size_t)w >= bufLen - off) return false;
    off += (size_t)w;
  }
  return off > 0;
}

static bool pulsesFromCsv(const char* s, uint16_t* out, uint16_t* n, uint16_t maxN) {
  if (!s || !*s) return false;
  uint16_t c = 0;
  const char* p = s;
  while (*p && c < maxN) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    char* end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p) break;
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    out[c++] = (uint16_t)v;
    p = end;
  }
  *n = c;
  return c >= 5;
}

bool RfCapture::learnKey(int idx, bool (*saveFn)(int, const char*)) {
  if (idx < 0 || idx >= RF_KEY_COUNT) return false;
  Serial.printf("[RF] 学习按键 %d：请短按遥控对应键...\n", idx);
  if (!capture()) return false;

  uint16_t frame[RF_KEY_MAX_PULSES];
  uint16_t fn = 0;
  if (!extractOneFrame(frame, &fn, RF_KEY_MAX_PULSES)) {
    Serial.println("[RF] 学习失败：提不出单帧");
    return false;
  }

  memcpy(keys_[idx], frame, fn * sizeof(uint16_t));
  keyLen_[idx] = fn;

  char csv[RF_KEY_MAX_PULSES * 6];
  if (!csvFromPulses(frame, fn, csv, sizeof(csv))) {
    Serial.println("[RF] 学习失败：CSV 过长");
    return false;
  }
  Serial.printf("[RF] 按键 %d 单帧 %u 脉冲: %s...\n", idx, fn, csv);
  if (saveFn) {
    if (!saveFn(idx, csv)) {
      Serial.println("[RF] 保存 NVS 失败");
      return false;
    }
    Serial.println("[RF] 已保存到 NVS");
  }
  return true;
}

bool RfCapture::setKeyFromCsv(int idx, const char* csv) {
  if (idx < 0 || idx >= RF_KEY_COUNT) return false;
  uint16_t n = 0;
  if (!pulsesFromCsv(csv, keys_[idx], &n, RF_KEY_MAX_PULSES)) {
    keyLen_[idx] = 0;
    return false;
  }
  keyLen_[idx] = n;
  return true;
}

bool RfCapture::keyValid(int idx) const {
  return idx >= 0 && idx < RF_KEY_COUNT && keyLen_[idx] >= 10;
}

uint16_t RfCapture::keyCount(int idx) const {
  if (idx < 0 || idx >= RF_KEY_COUNT) return 0;
  return keyLen_[idx];
}

void RfCapture::exportKeyCsv(int idx) const {
  if (idx < 0 || idx >= RF_KEY_COUNT || keyLen_[idx] == 0) return;
  static const char* names[4] = {"open", "close", "stop", "lock"};
  Serial.printf("RFDATA %d %s %u ", idx, names[idx], keyLen_[idx]);
  for (uint16_t i = 0; i < keyLen_[idx]; i++) {
    if (i) Serial.print(',');
    Serial.print(keys_[idx][i]);
  }
  Serial.println();
}

bool RfCapture::playFrame(const uint16_t* p, uint16_t n, uint8_t repeats) {
  if (txPin_ < 0 || !p || n < 5) return false;
  if (repeats == 0) repeats = 1;
  Serial.printf("[RF] 发射 TX=GPIO%d x%u 帧, %u 脉冲\n", txPin_, repeats, n);

  pinMode(txPin_, OUTPUT);
  digitalWrite(txPin_, LOW);
  delayMicroseconds(100);

  for (uint8_t r = 0; r < repeats; r++) {
    for (uint16_t i = 0; i < n; i++) {
      // OOK：偶数段为高（载波开），奇数为低；帧内长间隔当低电平保持
      bool high = (i % 2) == 0;
      digitalWrite(txPin_, high ? HIGH : LOW);
      uint16_t d = p[i];
      if (d >= RF_INTER_FRAME_MIN_US) {
        digitalWrite(txPin_, LOW);
        delayMicroseconds(d);
      } else {
        delayMicroseconds(d);
      }
    }
    digitalWrite(txPin_, LOW);
    if (r + 1 < repeats) delayMicroseconds(RF_FRAME_GAP_US);
  }
  digitalWrite(txPin_, LOW);
  Serial.println("[RF] 发射完成");
  return true;
}

bool RfCapture::carrierLoopback(uint32_t carrierMs) {
  if (rxPin_ < 0 || txPin_ < 0) return false;
  if (carrierMs < 100) carrierMs = 100;
  if (carrierMs > 3000) carrierMs = 3000;

  Serial.printf("[RF] ==== CARRIER LOOP TX=GPIO%d RX=GPIO%d high %ums ====\n",
                txPin_, rxPin_, carrierMs);
  pinMode(txPin_, OUTPUT);
  digitalWrite(txPin_, LOW);

  rfIdx = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);
  Serial.println("[RF] RX 预热 2000ms...");
  delay(2000);

  uint16_t warmN = rfIdx;
  rfCapturing = false;
  delayMicroseconds(50);
  rfIdx = 0;
  rfLastChangeUs = micros();
  rfCapturing = true;
  Serial.printf("[RF] 预热边沿 %u；TX 拉高 %ums\n", warmN, carrierMs);

  digitalWrite(txPin_, HIGH);
  delay(carrierMs);
  digitalWrite(txPin_, LOW);

  uint32_t waitStart = millis();
  while (rfCapturing && (millis() - waitStart) < 600) {
    if (rfIdx >= 5 && (micros() - rfLastChangeUs) > 30000) break;
    delay(1);
  }
  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(rxPin_));

  uint16_t rxN = rfIdx;
  for (uint16_t i = 0; i < rxN && i < RF_CAPTURE_MAX_PULSES; i++)
    pulses_[i] = rfPulseBuf[i];
  count_ = rxN;

  Serial.printf("[RF] 载波期间 RX 收到 %u 脉冲\n", rxN);
  Serial.print("[RX] raw:");
  for (uint16_t i = 0; i < rxN && i < 40; i++) {
    Serial.print(' ');
    Serial.print(pulses_[i]);
  }
  Serial.println();

  // 真·载波耦合时超再生会被打满（数百边沿）；几十个 4µs 碎脉冲只是底噪
  if (rxN >= 80) {
    Serial.println("[RF] CARRIER PASS：TX 载波能被 RX 听到 → 模块在发射");
    return true;
  }
  if (rxN >= 20) {
    Serial.println("[RF] CARRIER WEAK：仅少量边沿，疑似底噪，天线未有效耦合");
    Serial.println("[RF] 请把 TX/RX 天线靠近到 3~5cm 再试 rfcloop");
    return false;
  }
  Serial.println("[RF] CARRIER FAIL：持续高电平 RX 仍几乎无信号");
  Serial.println("[RF] 查：天线是否靠近、TX 天线是否接好、模块是否 315");
  return false;
}

void RfCapture::carrierTest(uint32_t ms) {
  if (txPin_ < 0) return;
  if (ms < 50) ms = 50;
  if (ms > 5000) ms = 5000;
  pinMode(txPin_, OUTPUT);
  Serial.printf("[RF] carrier: GPIO%d HIGH %ums（万用表测 DATA 应≈3.3V）\n",
                txPin_, ms);
  digitalWrite(txPin_, HIGH);
  delay(ms);
  digitalWrite(txPin_, LOW);
  Serial.println("[RF] carrier done, GPIO low");
}

bool RfCapture::playRaw(const char* pulseCsv, uint8_t repeats) {
  uint16_t p[RF_KEY_MAX_PULSES];
  uint16_t n = 0;
  if (!pulsesFromCsv(pulseCsv, p, &n, RF_KEY_MAX_PULSES)) return false;
  return playFrame(p, n, repeats);
}

bool RfCapture::playKey(int idx) {
  if (!keyValid(idx)) {
    Serial.printf("[RF] 按键 %d 未学习\n", idx);
    return false;
  }
  return playFrame(keys_[idx], keyLen_[idx], RF_PLAY_REPEATS);
}

static int compareFrames(const uint16_t* tx, uint16_t nTx,
                         const uint16_t* rx, uint16_t nRx,
                         uint16_t* firstDiff) {
  uint16_t n = nTx < nRx ? nTx : nRx;
  if (n > 240) n = 240;
  uint16_t bad = 0;
  if (firstDiff) *firstDiff = 0xFFFF;
  for (uint16_t i = 0; i < n; i++) {
    uint16_t a = tx[i], b = rx[i];
    uint16_t mx = a > b ? a : b;
    uint16_t d = a > b ? a - b : b - a;
    uint16_t tol = mx / 4;
    if (tol < 80) tol = 80;
    if (d > tol) {
      if (firstDiff && *firstDiff == 0xFFFF) *firstDiff = i;
      bad++;
    }
  }
  return (int)n - (int)bad;
}

bool RfCapture::loopbackKey(int idx, uint8_t repeats) {
  if (idx < 0 || idx >= RF_KEY_COUNT || !keyValid(idx)) {
    Serial.printf("[RF] loopback：按键 %d 未学习\n", idx);
    return false;
  }
  if (rxPin_ < 0 || txPin_ < 0) {
    Serial.println("[RF] loopback：RX/TX 引脚未配置");
    return false;
  }
  if (repeats == 0) repeats = 1;
  if (repeats > 6) repeats = 6;

  const uint16_t* txP = keys_[idx];
  uint16_t txN = keyLen_[idx];

  Serial.printf("[RF] ==== LOOPBACK key=%d TX=GPIO%d RX=GPIO%d x%u ====\n",
                idx, txPin_, rxPin_, repeats);
  Serial.printf("[RF] 发射帧长 %u；请将 TX 天线靠近 RX 天线（5~10cm）\n", txN);

  // 1) 先挂 RX，给超再生/AGC 足够预热（用户反馈需较长时间才稳）
  rfIdx = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);
  Serial.println("[RF] RX 启动，预热 2000ms（等接收稳定）...");
  delay(2000);

  // 2) 丢掉预热噪声，确认 RX 仍在监听
  uint16_t warmNoise = rfIdx;
  rfCapturing = false;
  delayMicroseconds(50);
  rfIdx = 0;
  rfLastChangeUs = micros();
  rfCapturing = true;
  Serial.printf("[RF] 预热期间收到 %u 边沿（噪声可忽略）；RX 就绪\n", warmNoise);

  // 3) 空帧唤醒发射通路（不计入对比），再正式发
  {
    uint16_t warm[8] = {100, 100, 400, 400, 100, 100, 400, 400};
    playFrame(warm, 8, 1);
    delay(100);
  }
  rfCapturing = false;
  delayMicroseconds(50);
  rfIdx = 0;
  rfLastChangeUs = micros();
  rfCapturing = true;
  Serial.println("[RF] 开始正式发射...");

  bool txOk = playFrame(txP, txN, repeats);

  // 4) 等帧间静默结束
  uint32_t waitStart = millis();
  while (rfCapturing && (millis() - waitStart) < 1200) {
    if (rfIdx >= 10 && (micros() - rfLastChangeUs) > 40000) break;
    delay(1);
  }
  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(rxPin_));
  delay(2);

  uint16_t rxN = rfIdx;
  for (uint16_t i = 0; i < rxN && i < RF_CAPTURE_MAX_PULSES; i++) {
    pulses_[i] = rfPulseBuf[i];
  }
  count_ = rxN;

  if (!txOk) {
    Serial.println("[RF] LOOPBACK FAIL：发射调用失败");
    return false;
  }

  if (rxN < 8) {
    Serial.printf("[RF] LOOPBACK FAIL：RX 只收到 %u 个脉冲（发射可能未出信号）\n",
                  rxN);
    Serial.println("[RF] 排查：TX 天线是否接好、模块供电、天线靠近 RX、GPIO 是否 26");
    Serial.print("[RX] raw:");
    for (uint16_t i = 0; i < rxN; i++) {
      Serial.print(' ');
      Serial.print(pulses_[i]);
      if (i + 1 < rxN) Serial.print(',');
    }
    Serial.println();
    return false;
  }

  // 取 RX 第一段（到第一个 >= RF_INTER_FRAME_MIN_US 为止）
  uint16_t rxFrame[RF_KEY_MAX_PULSES];
  uint16_t rfN = 0;
  for (uint16_t i = 0; i < rxN && rfN < RF_KEY_MAX_PULSES; i++) {
    uint16_t v = pulses_[i];
    if (v >= RF_INTER_FRAME_MIN_US) break;
    rxFrame[rfN++] = v;
  }
  if (rfN < 8) {
    // 容错：整段当一帧
    rfN = rxN < RF_KEY_MAX_PULSES ? rxN : RF_KEY_MAX_PULSES;
    for (uint16_t i = 0; i < rfN; i++) rxFrame[i] = pulses_[i];
  }

  uint16_t firstDiff = 0xFFFF;
  int good = compareFrames(txP, txN, rxFrame, rfN, &firstDiff);
  int cmpN = txN < rfN ? (int)txN : (int)rfN;
  if (cmpN > 240) cmpN = 240;
  int bad = cmpN - good;
  int pct = cmpN > 0 ? (good * 100 / cmpN) : 0;

  Serial.printf("[RF] RX 总脉冲 %u，提取首帧 %u；TX 帧 %u\n", rxN, rfN, txN);
  Serial.printf("[RF] 对比 %d 点：匹配 %d / 不匹配 %d（%d%%）\n", cmpN, good, bad,
                pct);
  if (firstDiff != 0xFFFF && firstDiff < cmpN) {
    Serial.printf("[RF] 首个差异 @%u: TX=%u RX=%u\n", firstDiff,
                  txP[firstDiff], rxFrame[firstDiff]);
  }

  Serial.print("[TX] frame:");
  for (uint16_t i = 0; i < txN && i < 40; i++) {
    Serial.print(' ');
    Serial.print(txP[i]);
    if (i + 1 < (txN < 40 ? txN : 40)) Serial.print(',');
  }
  if (txN > 40) Serial.print(" ...");
  Serial.println();

  Serial.print("[RX] frame:");
  for (uint16_t i = 0; i < rfN && i < 40; i++) {
    Serial.print(' ');
    Serial.print(rxFrame[i]);
    if (i + 1 < (rfN < 40 ? rfN : 40)) Serial.print(',');
  }
  if (rfN > 40) Serial.print(" ...");
  Serial.println();

  if (pct >= 85 && bad <= 6) {
    Serial.println("[RF] LOOPBACK PASS：发射链路正常，波形与学习码一致");
    Serial.println("[RF] 若门仍不认：距离/天线/是否滚码/接收端频偏");
    return true;
  }
  if (pct >= 50) {
    Serial.println("[RF] LOOPBACK WEAK：能收到但波形偏差大（干扰/极性/帧对齐问题）");
    return false;
  }
  Serial.println("[RF] LOOPBACK FAIL：收到的与发送的不像同一码");
  Serial.println("[RF] 可能：收的是噪声、天线未耦合、或学习帧本身不对");
  return false;
}

// 在 RX 流上滑动找与 TX 最吻合的帧对齐（近场/噪声下比硬截首帧稳）
static int bestAlignedMatch(const uint16_t* tx, uint16_t nTx,
                            const uint16_t* rx, uint16_t nRx,
                            uint16_t* outOff, uint16_t* outCmpN) {
  if (!tx || !rx || nTx < 8 || nRx < 8) return -1;
  uint16_t win = nTx;
  if (win > 240) win = 240;
  if (win > nRx) win = nRx;

  int bestGood = -1;
  uint16_t bestOff = 0;
  uint16_t bestN = 0;
  uint16_t maxOff = (nRx > win) ? (nRx - win) : 0;
  if (maxOff > 200) maxOff = 200;

  for (uint16_t off = 0; off <= maxOff; off++) {
    uint16_t n = nTx;
    if (n > nRx - off) n = nRx - off;
    if (n > 240) n = 240;
    if (n < 16) break;
    uint16_t good = 0;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t a = tx[i], b = rx[off + i];
      uint16_t mx = a > b ? a : b;
      uint16_t d = a > b ? a - b : b - a;
      uint16_t tol = mx / 4;
      if (tol < 80) tol = 80;
      if (d <= tol) good++;
    }
    // 略偏向前段对齐（off 小时更可能是首帧）
    int score = (int)good * 1000 - (int)off;
    int bestScore = (bestGood >= 0) ? bestGood * 1000 - (int)bestOff : -1;
    if (score > bestScore) {
      bestGood = (int)good;
      bestOff = off;
      bestN = n;
    }
  }
  if (outOff) *outOff = bestOff;
  if (outCmpN) *outCmpN = bestN;
  return bestGood;
}

bool RfCapture::benchLoopbackKey(int idx, uint8_t rounds, uint32_t intervalMs) {
  if (idx < 0 || idx >= RF_KEY_COUNT || !keyValid(idx)) {
    Serial.printf("[RF] bench：按键 %d 未学习\n", idx);
    return false;
  }
  if (rxPin_ < 0 || txPin_ < 0) {
    Serial.println("[RF] bench：RX/TX 引脚未配置");
    return false;
  }
  if (rounds == 0) rounds = 1;
  if (rounds > 20) rounds = 20;
  if (intervalMs < 2000) intervalMs = 2000;
  if (intervalMs > 60000) intervalMs = 60000;

  const uint16_t* txP = keys_[idx];
  uint16_t txN = keyLen_[idx];

  Serial.printf("[RF] ==== RF BENCH key=%d rounds=%u interval=%ums ====\n",
                idx, rounds, intervalMs);
  Serial.printf("[RF] TX=GPIO%d RX=GPIO%d；TX 帧长 %u；%u 脉冲\n",
                txPin_, rxPin_, txN, txN);
  Serial.println("[RF] RX 先预热，之后每轮发一次并与学习码对齐对比...");

  // RX 全程挂着（模块 AGC 稳定），仅在发瞬间 arm 抓包缓冲
  rfIdx = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);
  Serial.println("[RF] RX 预热 2500ms...");
  delay(2500);

  uint16_t warmN = rfIdx;
  Serial.printf("[RF] 预热噪声边沿 %u（忽略）；开始周期测试\n", warmN);

  int passCnt = 0, weakCnt = 0, failCnt = 0;
  int sumPct = 0;
  int lastPct = -1;
  bool stable = true;

  for (uint8_t r = 1; r <= rounds; r++) {
    // 清缓冲后 arm：只记本轮 TX 附近信号
    rfCapturing = false;
    delayMicroseconds(50);
    rfIdx = 0;
    rfLastChangeUs = micros();
    rfCapturing = true;

    uint32_t t0 = millis();
    Serial.printf("[RF] ---- 轮次 %u/%u ----\n", r, rounds);
    playFrame(txP, txN, RF_PLAY_REPEATS);

    uint32_t waitStart = millis();
    while (rfCapturing && (millis() - waitStart) < 1500) {
      if (rfIdx >= 8 && (micros() - rfLastChangeUs) > 50000) break;
      delay(1);
    }
    rfCapturing = false;

    uint16_t rxN = rfIdx;
    if (rxN > RF_CAPTURE_MAX_PULSES) rxN = RF_CAPTURE_MAX_PULSES;
    for (uint16_t i = 0; i < rxN; i++) pulses_[i] = rfPulseBuf[i];
    count_ = rxN;

    if (rxN < 8) {
      failCnt++;
      stable = false;
      Serial.printf("[RF] 轮 %u: RX 仅 %u 脉冲 → FAIL（本收不到）\n", r, rxN);
    } else {
      uint16_t off = 0, cmpN = 0;
      int good = bestAlignedMatch(txP, txN, pulses_, rxN, &off, &cmpN);
      if (good < 0 || cmpN == 0) {
        failCnt++;
        stable = false;
        Serial.printf("[RF] 轮 %u: RX %u 脉冲，无法对齐 → FAIL\n", r, rxN);
      } else {
        int bad = (int)cmpN - good;
        int pct = good * 100 / (int)cmpN;
        sumPct += pct;
        if (pct < 70) {
          failCnt++;
          stable = false;
        } else if (pct < 85) {
          weakCnt++;
          stable = false;
        } else {
          passCnt++;
        }
        if (lastPct >= 0 && (pct - lastPct > 15 || lastPct - pct > 15)) {
          stable = false;
        }
        lastPct = pct;
        Serial.printf("[RF] 轮 %u: RX %u 脉冲, 对齐 off=%u, 对比 %u 点: 好 %d / 差 %d → %d%%\n",
                      r, rxN, off, cmpN, good, bad, pct);
        // 打印对齐后前后段，便于肉眼比波形
        Serial.print("[TX] ");
        for (uint16_t i = 0; i < txN && i < 24; i++) {
          if (i) Serial.print(',');
          Serial.print(txP[i]);
        }
        if (txN > 24) Serial.print(",...");
        Serial.println();
        Serial.print("[RX] ");
        for (uint16_t i = off; i < rxN && i < off + 24; i++) {
          if (i > off) Serial.print(',');
          Serial.print(pulses_[i]);
        }
        if (rxN > off + 24) Serial.print(",...");
        Serial.println();
        if (bad > 0) {
          uint16_t shown = 0;
          for (uint16_t i = 0; i < cmpN && shown < 5; i++) {
            uint16_t a = txP[i], b = pulses_[off + i];
            uint16_t mx = a > b ? a : b;
            uint16_t d = a > b ? a - b : b - a;
            uint16_t tol = mx / 4;
            if (tol < 80) tol = 80;
            if (d > tol) {
              Serial.printf("[RF]   diff@%u TX=%u RX=%u\n", i, a, b);
              shown++;
            }
          }
        }
      }
    }

    // 保持 10s 周期；RX 中断仍挂着，下一轮再 arm
    uint32_t spent = millis() - t0;
    if (r < rounds && spent < intervalMs) {
      uint32_t wait = intervalMs - spent;
      Serial.printf("[RF] 等待 %ums 后下一轮...\n", wait);
      uint32_t s = millis();
      // 期间丢弃噪声：capturing=false，模块仍在收
      while (millis() - s < wait) delay(20);
      rfIdx = 0;
    }
  }

  detachInterrupt(digitalPinToInterrupt(rxPin_));
  rfCapturing = false;

  int avg = (passCnt + weakCnt + failCnt) > 0
                ? sumPct / (passCnt + weakCnt + failCnt)
                : 0;
  Serial.println("[RF] ========== BENCH 汇总 ==========");
  Serial.printf("[RF] PASS(≥85%%)=%d  WEAK(70-84%%)=%d  FAIL(<70%%或无信号)=%d\n",
                passCnt, weakCnt, failCnt);
  Serial.printf("[RF] 平均匹配 %d%%；最后一带 %d%%\n", avg, lastPct);
  if (passCnt == rounds && stable) {
    Serial.println("[RF] BENCH PASS：信号稳定，收到波形与学习码一致");
    Serial.println("[RF] 可用 rfplay 0 对真实门测试（天线指向门接收端 1~3m）");
    return true;
  }
  if (passCnt + weakCnt >= (rounds + 1) / 2 && avg >= 70) {
    Serial.println("[RF] BENCH WEAK：多数轮次能对上，但不够稳定");
    Serial.println("[RF] 查：天线距离/方向、供电纹波、学习帧毛刺");
    return false;
  }
  Serial.println("[RF] BENCH FAIL：与学习码不稳定一致或收不到");
  return false;
}

uint32_t RfCapture::hash() const {
  uint32_t h = 0x811c9dc5;
  for (uint16_t i = 0; i < count_; i++) {
    h ^= pulses_[i];
    h *= 0x01000193;
  }
  return h;
}

void RfCapture::dump(uint16_t maxShow) const {
  if (count_ == 0) {
    Serial.println("[RF] (空)");
    return;
  }
  Serial.print("[RF] ");
  uint16_t show = (count_ < maxShow) ? count_ : maxShow;
  for (uint16_t i = 0; i < show; i++) {
    Serial.print(pulses_[i]);
    if (i + 1 < show) Serial.print(",");
  }
  if (count_ > maxShow) Serial.printf(" ...(+%u)", count_ - maxShow);
  Serial.println();
}
