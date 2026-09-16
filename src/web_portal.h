#pragma once
#include <Arduino.h>
#include "ble_tracker.h"
#include "door_fsm.h"
#include "config_store.h"
#include "ble_scan.h"
#include "config.h"

// SoftAP + 手机网页：设置车机蓝牙 MAC / BLE 特征 / 查看状态 / 手动开关
class WebPortal {
 public:
  void begin(ConfigStore* store, BleTracker* bt, DoorFsm* door, BleScanTool* ble,
             bool enableAp = true);
  void loop();
  String apSsid() const { return apSsid_; }
  IPAddress apIp() const;
  bool apActive() const { return apActive_; }
  bool startAp();
  void stopAp();

  // 跟踪模式：0=BLE, 1=Classic
  int trackMode() const { return trackMode_; }
  void setTrackMode(int mode);

 private:
  void setupRoutes();
  String pageHtml() const;

  ConfigStore* store_ = nullptr;
  BleTracker* bt_ = nullptr;
  DoorFsm* door_ = nullptr;
  BleScanTool* ble_ = nullptr;
  String apSsid_;
  bool apActive_ = false;
  bool serverStarted_ = false;
  int trackMode_ = TRACK_MODE_DEFAULT;
};
