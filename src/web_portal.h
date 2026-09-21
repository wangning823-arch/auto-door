#pragma once
#include <Arduino.h>
#include <DNSServer.h>
#include "ble_tracker.h"
#include "door_fsm.h"
#include "config_store.h"
#include "ble_scan.h"
#include "ble_bond.h"
#include "config.h"
#include "nfc_reader.h"

// SoftAP + 手机网页：设置车机蓝牙 MAC / BLE 特征 / 查看状态 / 手动开关
class WebPortal {
 public:
  void begin(ConfigStore* store, BleTracker* bt, DoorFsm* door, BleScanTool* ble,
             NfcReader* nfc, bool enableAp = true);
  void loop();
  String apSsid() const { return apSsid_; }
  IPAddress apIp() const;
  bool apActive() const { return apActive_; }
  bool startAp();
  void stopAp();

  // 跟踪模式：0=BLE, 1=Classic
  int trackMode() const { return trackMode_; }
  void setTrackMode(int mode);

  // SoftAP 开着（调试/配置）时：主循环应把射频让给 WiFi，暂停阻塞式 BLE 扫描
  bool wifiRfPriority() const { return apActive_; }
  // SoftAP 启动后的静默窗口是否仍未结束
  bool rfQuietActive() const;

 private:
  void setupRoutes();
  String pageHtml() const;

  ConfigStore* store_ = nullptr;
  BleTracker* bt_ = nullptr;
  DoorFsm* door_ = nullptr;
  BleScanTool* ble_ = nullptr;
  NfcReader* nfc_ = nullptr;
  String apSsid_;
  bool apActive_ = false;
  bool serverStarted_ = false;
  int trackMode_ = TRACK_MODE_DEFAULT;
  uint32_t apQuietUntilMs_ = 0;
  // 强制门户：手机连上无外网 AP 时，DNS 全指到 192.168.4.1，系统才会弹/可开配置页
  DNSServer dns_;
  bool dnsOn_ = false;
};