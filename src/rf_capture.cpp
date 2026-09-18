#include "rf_capture.h"

// 全局脉冲缓冲（ISR 写入，主循环读取）
static volatile uint16_t rfPulseBuf[RF_CAPTURE_MAX_PULSES];
static volatile uint32_t rfLastChangeUs = 0;
static volatile uint16_t rfIdx = 0;
static volatile bool rfCapturing = false;

static void IRAM_ATTR rfIsr() {
  if (!rfCapturing) return;
  uint32_t now = micros();
  uint32_t dur = now - rfLastChangeUs;
  rfLastChangeUs = now;

  if (dur >= RF_CAPTURE_GAP_US) {
    if (rfIdx > 10) rfCapturing = false;
    return;
  }
  if (rfIdx < RF_CAPTURE_MAX_PULSES) {
    rfPulseBuf[rfIdx++] = (dur > 65535) ? 65535 : (uint16_t)dur;
  }
}

void RfCapture::begin(int pin) {
  pin_ = pin;
  pinMode(pin_, INPUT);
}

bool RfCapture::capture(uint32_t timeoutMs) {
  if (pin_ < 0) return false;

  // 保存上一次数据用于对比
  if (count_ > 0) {
    memcpy(lastPulses_, pulses_, count_ * sizeof(uint16_t));
    lastCount_ = count_;
    hasLast_ = true;
  }

  // 重置状态
  rfIdx = 0;
  rfCapturing = true;
  rfLastChangeUs = micros();
  count_ = 0;

  Serial.printf("[RF] 等待信号... (DATA=GPIO%d, 超时 %ums)\n", pin_, timeoutMs);
  Serial.println("[RF] 请按遥控器按钮...");

  // attachInterrupt
  attachInterrupt(digitalPinToInterrupt(pin_), rfIsr, CHANGE);

  uint32_t start = millis();
  while (rfCapturing && (millis() - start) < timeoutMs) {
    delay(1);
  }
  rfCapturing = false;
  detachInterrupt(digitalPinToInterrupt(pin_));
  delay(10);

  count_ = rfIdx;
  for (uint16_t i = 0; i < count_; i++) {
    pulses_[i] = rfPulseBuf[i];
  }

  if (count_ < 10) {
    Serial.println("[RF] 抓包失败：未收到有效信号");
    Serial.println("[RF] 检查：模块接线、供电、遥控器电量");
    return false;
  }

  Serial.printf("[RF] 抓包成功：%u 个脉冲\n", count_);
  // 完整脉冲序列，供 PC 工具解析画波形
  Serial.print("[RF] pulses:");
  for (uint16_t i = 0; i < count_; i++) {
    Serial.print(' ');
    Serial.print(pulses_[i]);
    if (i + 1 < count_) Serial.print(',');
  }
  Serial.println();

  // 有上次数据则自动对比
  if (hasLast_) {
    Serial.println("[RF] ---- 与上次对比 ----");
    compareWithLast();
  } else {
    Serial.println("[RF] 第一次抓包完成，再执行 rfcap 抓第二次对比");
  }

  return true;
}

// 检测重复帧长（固定码/重复帧遥控）：pulses[i] ≈ pulses[i+L]
static uint16_t detectFrameLen(const uint16_t* p, uint16_t n) {
  if (n < 32) return n;
  uint16_t bestL = 0;
  uint16_t bestScore = 0;
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
    if (cmp >= 16 && score * 10 >= cmp * 8) {  // ≥80% 匹配
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

  // 按重复帧对齐：按压时长不同会导致总长不同，先取单帧再比
  uint16_t la = detectFrameLen(lastPulses_, lastCount_);
  uint16_t lb = detectFrameLen(pulses_, count_);
  uint16_t frameLen = (la < lb) ? la : lb;
  if (frameLen > 240) frameLen = 240;  // 单帧不会太长

  Serial.printf("[RF] 帧长估计: %u / %u（按压次数不同不影响单帧对比）\n", la, lb);

  uint16_t mismatches = 0;
  uint16_t cmpN = frameLen;
  for (uint16_t i = 0; i < cmpN; i++) {
    uint16_t a = pulses_[i];
    uint16_t b = lastPulses_[i];
    uint16_t diff = (a > b) ? (a - b) : (b - a);
    uint16_t mx = (a > b) ? a : b;
    uint16_t tol = mx / 4;
    if (tol < 80) tol = 80;
    if (diff > tol) {
      mismatches++;
      if (mismatches <= 5) {
        Serial.printf("[RF] 差异 #%u: %u vs %u\n", i, a, b);
      }
    }
  }

  if (mismatches == 0) {
    Serial.println("[RF] ========== 码相同 ==========");
    Serial.println("[RF] 结论：固定码，可直接克隆回放");
    return true;
  }

  Serial.printf("[RF] 单帧内 %u 处脉冲不同 (共比较 %u)\n", mismatches, cmpN);
  Serial.println("[RF] ========== 码不同 ==========");
  Serial.println("[RF] 结论：可能是滚码，需要解码算法");
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
