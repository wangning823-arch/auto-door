#pragma once
#include <Arduino.h>

// NVS 持久化：车机 MAC、可选参数
class ConfigStore {
 public:
  void begin();
  String loadMac(const char* defaultMac);
  bool saveMac(const String& mac);
  bool clearMac();

  // SoftAP 是否启用（默认开；网页可关，关掉后 BT Inquiry 独占射频）
  bool loadWifiEnabled(bool defaultOn = true);
  bool saveWifiEnabled(bool on);

  // BLE 特征过滤：如 MiCarCDB8 或 MAC 前缀；空=未设置
  String loadBleFilter();
  bool saveBleFilter(const String& f);

  // 跟踪模式：0=BLE, 1=经典蓝牙（默认 BLE）
  int loadTrackMode(int defaultMode = 0);
  bool saveTrackMode(int mode);

  // NFC 授权卡 UID（十六进制，如 "04A1B2C3"）；空=未注册
  String loadNfcUid();
  bool saveNfcUid(const String& uid);
  bool clearNfcUid();

 private:
  bool ready_ = false;
};
