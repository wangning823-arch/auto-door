#include "rf_capture.h"

static volatile uint16_t rfPulseBuf[RF_CAPTURE_MAX_PULSES];
static volatile uint32_t rfLastChangeUs = 0;
static volatile uint16_t rfIdx = 0;
static volatile bool rfCapturing = false;

static void IRAM_ATTR rfIsr() {
  if (!rfCapturing) return;
  uint32_t now = micros();
  uint32_t dur = now - rfLastChangeUs;
  rfLastChangeUs = now;

  if (rfIdx == 0 && dur >= RF_CAPTURE_GAP_US) {
    return;
  }
  if (dur >= RF_CAPTURE_GAP_US) {
    if (rfIdx > 10) rfCapturing = false;
    return;
  }
  if (rfIdx < RF_CAPTURE_MAX_PULSES) {
    rfPulseBuf[rfIdx++] = (dur > 65535) ? 65535 : (uint16_t)dur;
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
  rfCapturing = true;
  rfLastChangeUs = micros();
  count_ = 0;

  Serial.printf("[RF] 等待信号... (RX=GPIO%d, 超时 %ums)\n", rxPin_, timeoutMs);
  Serial.println("[RF] 请短按遥控器（5~10cm 对准天线）...");

  attachInterrupt(digitalPinToInterrupt(rxPin_), rfIsr, CHANGE);

  uint32_t start = millis();
  uint16_t lastPrinted = 0;
  while ((millis() - start) < timeoutMs) {
    if (!rfCapturing) break;
    uint16_t n = rfIdx;
    if (n >= 10 && lastPrinted == 0) {
      Serial.printf("[RF] 已收到 %u 脉冲...\n", n);
      lastPrinted = n;
    }
    if (n >= 10 && rfLastChangeUs > 0) {
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

  if (count_ < 10) {
    Serial.printf("[RF] 抓包失败：仅 %u 个脉冲\n", count_);
    return false;
  }

  Serial.printf("[RF] 抓包成功：%u 个脉冲\n", count_);
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
  return true;
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
  uint16_t n = 0;
  for (uint16_t i = 0; i < count_ && n < maxN; i++) {
    uint16_t v = pulses_[i];
    if (v >= RF_INTER_FRAME_MIN_US) {
      if (n > 0) break;  // 第一帧结束
      continue;          // 开头的长间隔跳过
    }
    out[n++] = v;
  }
  *outN = n;
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
