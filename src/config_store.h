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

  // 家庭/车库路由器 STA（OTA 无线烧录）；空 SSID=未配置
  String loadStaSsid();
  String loadStaPass();
  bool saveSta(const String& ssid, const String& pass);
  bool clearSta();

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

  // RF 固定码按键 0..3（CSV 脉冲）；空=未学习
  String loadRfKey(int idx);
  bool saveRfKey(int idx, const char* csv);
  bool clearRfKey(int idx);

  // rfauto：上电恢复周期发射（默认关）
  bool loadRfAuto(bool defaultOn = false);
  bool saveRfAuto(bool on);

  // 经典蓝牙周期 Inquiry（默认：经典模式=开，BLE模式=关）
  bool loadAutoTrack(bool defaultOn);
  bool saveAutoTrack(bool on);

 private:
  bool ready_ = false;
};
