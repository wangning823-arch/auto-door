#include "ble_tracker.h"
#include "config.h"
#include "BluetoothSerial.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Classic Bluetooth not enabled"
#endif

static BluetoothSerial SerialBT;
static BleTracker* gTracker = nullptr;
static bool gBtReady = false;

static String macToStr(const uint8_t* bda) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1],
           bda[2], bda[3], bda[4], bda[5]);
  return String(buf);
}

static String codToKind(uint32_t cod) {
  uint32_t major = (cod >> 8) & 0x1F;
  switch (major) {
    case 0x01: return "电脑";
    case 0x02: return "手机";
    case 0x03: return "网络";
    case 0x04: return "音频";
    case 0x05: return "外设";
    case 0x06: return "影像";
    case 0x07: return "穿戴";
    default: return "";
  }
}

// 从 EIR 完整数据里抠本地名称
static bool nameFromEir(const uint8_t* eir, int eirLen, String& out) {
  if (!eir || eirLen <= 0) return false;
  int i = 0;
  while (i + 1 < eirLen) {
    uint8_t len = eir[i];
    if (len == 0) break;
    if (i + len >= eirLen) break;
    uint8_t type = eir[i + 1];
    if (type == 0x09 || type == 0x08) {  // Complete / Shortened Local Name
      int nlen = len - 1;
      if (nlen > 0) {
        char tmp[248] = {0};
        if (nlen > 247) nlen = 247;
        memcpy(tmp, &eir[i + 2], nlen);
        out = String(tmp);
        return out.length() > 0;
      }
    }
    i += len + 1;
  }
  return false;
}

static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (!gTracker) return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      String mac = macToStr(param->disc_res.bda);
      int rssi = -90;
      String name;
      uint32_t cod = 0;
      int nprop = param->disc_res.num_prop;
      for (int i = 0; i < nprop; i++) {
        esp_bt_gap_dev_prop_t* p = &param->disc_res.prop[i];
        if (!p || !p->val) continue;
        if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->len >= 1) {
          rssi = *(int8_t*)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
          name = String((const char*)p->val);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_COD && p->len >= 4) {
          cod = *(uint32_t*)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->len > 0) {
          String n2;
          if (nameFromEir((const uint8_t*)p->val, p->len, n2) && name.length() == 0) {
            name = n2;
          }
        }
      }
      if (name.length()) {
        gTracker->onClassicDevice(mac, rssi, name);
      } else {
        // 没带名称：先登记，再异步读远程名称
        gTracker->onClassicDevice(mac, rssi, "");
        uint8_t bda[6];
        for (int i = 0; i < 6; i++) {
          unsigned v = 0;
          sscanf(mac.c_str() + i * 3, "%02x", &v);
          bda[i] = (uint8_t)v;
        }
        esp_bt_gap_read_remote_name(bda);
      }
      break;
    }
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        String mac = macToStr(param->read_rmt_name.bda);
        String name = String((const char*)param->read_rmt_name.rmt_name);
        if (name.length()) {
          gTracker->onDeviceName(mac, name);
        }
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        gTracker->onInquiryDone();
      }
      break;
    }
    default:
      break;
  }
}

bool BleTracker::begin(const char* macStr) {
  gTracker = this;
  targetMac_ = String(macStr);
  targetMac_.toUpperCase();
  targetSet_ = (targetMac_.length() == 17);

  if (!gBtReady) {
    if (!SerialBT.begin("GarageDoor")) {
      Serial.println("[BT] SerialBT.begin FAILED");
      return false;
    }
    // 默认不可被搜索/不可连：避免关配对后仍被手机搜到 GarageDoor
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_gap_register_callback(gapCallback);
    gBtReady = true;
    Serial.println("[BT] Classic ready, NON_DISCOVERABLE (仅按需 inquiry)");
  }

  Serial.printf("[BT] target MAC %s -> %s\n", macStr, targetSet_ ? "OK" : "INVALID");
  nextInquiryMs_ = millis() + 1000;
  return true;
}

void BleTracker::setInquiryPaused(bool paused) {
  inquiryPaused_ = paused;
  if (paused) cancelActiveInquiry();
}

void BleTracker::setInquirySlow(bool slow) {
  inquirySlow_ = slow;
  if (slow) {
    // 让出射频给 SoftAP，但仍保留跟踪
    if (nextInquiryMs_ < millis() + 8000) nextInquiryMs_ = millis() + 8000;
  }
}

void BleTracker::cancelActiveInquiry() {
  if (!gBtReady) return;
  if (inquiryBusy_ && !discRunning_) {
    esp_bt_gap_cancel_discovery();
  }
}

int BleTracker::lastRssi() const {
  // 旧值会误导网页/状态：太久没扫到就当作丢失
  if (lastSeenMs_ != 0 && (millis() - lastSeenMs_) > 20000) return -127;
  return lastRssi_;
}

void BleTracker::startDiscovery(uint32_t durationMs) {
  if (!gBtReady) {
    Serial.println("[BT] startDiscovery: BT not ready");
    return;
  }
  // 用户主动扫描：先取消可能卡住的 inquiry
  esp_bt_gap_cancel_discovery();
  inquiryBusy_ = false;
  delay(50);

  discList_.clear();
  discRunning_ = true;
  discEndMs_ = millis() + durationMs;
  inquiryBusy_ = true;
  // length 单位 1.28s，0x01–0x30；用 8 ≈ 10s
  uint8_t len = (uint8_t)constrain((durationMs + 1279) / 1280, 2, 48);
  esp_err_t err =
      esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, len, 0);
  Serial.printf("[BT] inquiry start len=%u ret=%d (%s)\n", len, (int)err,
                err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) {
    inquiryBusy_ = false;
    discRunning_ = false;
  }
}

bool BleTracker::discoveryRunning() const {
  return discRunning_ && millis() <= discEndMs_;
}

void BleTracker::onClassicDevice(const String& mac, int rssi, const String& name) {
  String m = mac;
  m.toUpperCase();
  Serial.printf("[BT] FOUND %s rssi=%d name=%s\n", m.c_str(), rssi,
                name.length() ? name.c_str() : "(none)");

  if (discRunning_ && millis() <= discEndMs_) {
    bool found = false;
    for (auto& it : discList_) {
      if (it.mac == m) {
        if (rssi > it.rssi) it.rssi = rssi;
        if (it.name.length() == 0 && name.length()) it.name = name;
        found = true;
        break;
      }
    }
    if (!found && discList_.size() < 32) {
      discList_.push_back({m, rssi, name});
    }
  }

  if (targetSet_ && m == targetMac_) {
    if (rssi == 0) rssi = -70;
    lastRssi_ = rssi;
    missCount_ = 0;  // 扫到了，清零漏扫
    pushSample(true, rssi);
    computeSlope();
    classifyTrend(true, rssi);
    updateZone();
  }
}

void BleTracker::onDeviceName(const String& mac, const String& name) {
  String m = mac;
  m.toUpperCase();
  for (auto& it : discList_) {
    if (it.mac == m) {
      if (it.name.length() == 0) it.name = name;
      Serial.printf("[BT] name %s = %s\n", m.c_str(), name.c_str());
      return;
    }
  }
  // 名称先到、列表还没插入时也补一条
  if (discRunning_ && discList_.size() < 32) {
    discList_.push_back({m, -90, name});
  }
}

void BleTracker::onInquiryDone() {
  inquiryBusy_ = false;
  Serial.printf("[BT] inquiry stopped, list=%u miss=%u rssi=%d\n",
                (unsigned)discList_.size(), missCount_, lastRssi_);
  // 漏扫不清零：连续 3 轮未见才算不可见
  if (targetSet_ && !discRunning_) {
    if (missCount_ < 255) missCount_++;
    if (missCount_ >= 3) {
      pushSample(false, -127);
      computeSlope();
      classifyTrend(false, lastRssi_);
      updateZone();
    }
  }
  if (discRunning_ && millis() <= discEndMs_ && !inquiryPaused_) {
    inquiryBusy_ = true;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 3, 0);
  }
}

std::vector<BleDeviceItem> BleTracker::discoveryResults() const { return discList_; }

void BleTracker::pushSample(bool visible, int rssi) {
  hist_[histHead_] = visible ? (int8_t)constrain(rssi, -127, 0) : (int8_t)-127;
  histHead_ = (histHead_ + 1) % WIN;
  if (histCount_ < WIN) histCount_++;
}

void BleTracker::computeSlope() {
  if (histCount_ < 3) {
    slope_ = 0;
    return;
  }
  float sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
  int n = 0;
  for (int i = 0; i < histCount_; i++) {
    int idx = (histHead_ - histCount_ + i + WIN * 2) % WIN;
    if (hist_[idx] <= -127) continue;
    float x = (float)n;
    float y = (float)hist_[idx];
    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumXX += x * x;
    n++;
  }
  if (n < 3) {
    slope_ = 0;
    return;
  }
  float denom = n * sumXX - sumX * sumX;
  if (fabsf(denom) < 1e-6f) {
    slope_ = 0;
    return;
  }
  slope_ = (n * sumXY - sumX * sumY) / denom;
}

void BleTracker::classifyTrend(bool visible, int rssi) {
  uint32_t now = millis();
  if (visible) {
    if (!wasVisible_ && (lastSeenMs_ == 0 || (now - lastSeenMs_) > T_SILENT_GAP_MS)) {
      if (lastRssi_ <= -127 && rssi >= RSSI_OPEN) {
        trend_ = SignalTrend::SUDDEN_APPEAR;
      }
    }
    lastSeenMs_ = now;
    silentSinceMs_ = 0;
    wasVisible_ = true;
    if (trend_ != SignalTrend::SUDDEN_APPEAR) {
      if (slope_ >= SLOPE_MIN && lastRssi_ >= RSSI_OPEN) {
        trend_ = SignalTrend::GRADUAL_IN;
      } else if (slope_ <= -SLOPE_MIN && lastRssi_ <= RSSI_FADE) {
        trend_ = SignalTrend::GRADUAL_OUT;
      } else {
        trend_ = SignalTrend::STEADY;
      }
    }
  } else {
    if (wasVisible_) {
      if (lastRssi_ >= RSSI_FADE && slope_ > -SLOPE_MIN) {
        trend_ = SignalTrend::SUDDEN_LOSS;
      } else if (slope_ <= -SLOPE_MIN || lastRssi_ <= RSSI_FADE) {
        trend_ = SignalTrend::GRADUAL_OUT;
      }
      silentSinceMs_ = now;
      wasVisible_ = false;
    } else if (silentSinceMs_ != 0) {
      if (trend_ == SignalTrend::SUDDEN_LOSS && (now - silentSinceMs_) > T_CLEAR_MS) {
        trend_ = SignalTrend::UNKNOWN;
      }
    }
  }
}

void BleTracker::updateZone() {
  bool clearOk = silentSinceMs_ != 0 && (millis() - silentSinceMs_) >= T_CLEAR_MS;
  if (zone_ == CarZone::TRANSIT) {
    if (clearOk && trend_ == SignalTrend::GRADUAL_OUT) zone_ = CarZone::OUT;
    return;
  }
  if (lastRssi_ >= RSSI_OPEN &&
      (trend_ == SignalTrend::GRADUAL_IN || trend_ == SignalTrend::STEADY)) {
    zone_ = CarZone::IN_GARAGE;
    everInGarage_ = true;
  } else if (wasVisible_ || (lastSeenMs_ && (millis() - lastSeenMs_) < 5000)) {
    zone_ = CarZone::NEAR;
    if (trend_ == SignalTrend::GRADUAL_OUT || trend_ == SignalTrend::SUDDEN_LOSS) {
      zone_ = CarZone::TRANSIT;
    }
  } else if (clearOk) {
    zone_ = CarZone::OUT;
  } else if (trend_ == SignalTrend::SUDDEN_LOSS) {
    zone_ = CarZone::TRANSIT;
  } else {
    zone_ = CarZone::OUT;
  }
}

void BleTracker::loop() {
  if (discRunning_ && millis() > discEndMs_) {
    discRunning_ = false;
    inquiryBusy_ = false;
    Serial.printf("[BT] discovery done, %u devices\n", (unsigned)discList_.size());
  }

  // 显式暂停时才停后台跟踪；SoftAP 慢速模式仍要扫（否则手机连热点时车走了永远不关）
  if (!autoTrack_ || inquiryPaused_ || !gBtReady || !targetSet_ || discRunning_ ||
      inquiryBusy_) {
    return;
  }
  if (millis() >= nextInquiryMs_) {
    inquiryBusy_ = true;
    uint32_t gap = inquirySlow_ ? 15000 : 3000;
    nextInquiryMs_ = millis() + gap;
    uint8_t len = inquirySlow_ ? 1 : 2;  // 1≈1.28s，短一些少打网页
    esp_err_t err =
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, len, 0);
    Serial.printf("[BT] auto inquiry slow=%d ret=%d\n", (int)inquirySlow_,
                  (int)err);
    if (err != ESP_OK) inquiryBusy_ = false;
  }
}

bool BleTracker::seenRecently(uint32_t withinMs) const {
  return lastSeenMs_ != 0 && (millis() - lastSeenMs_) <= withinMs;
}

void BleTracker::markLeftForCloseEval() {}

String BleTracker::debugLine() const {
  char buf[180];
  snprintf(buf, sizeof(buf),
           "rssi=%d raw=%d slope=%.2f trend=%d zone=%d seen=%lu auto=%d slow=%d",
           lastRssi(), lastRssiRaw(), (double)slope_, (int)trend_, (int)zone_,
           (unsigned long)lastSeenMs_, (int)autoTrack_, (int)inquirySlow_);
  return String(buf);
}
