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

  // 家庭路由 STA（网页保存后 / 上电自动连；用于 espota 无线烧录）
  void startStaFromStore();
  void stopSta();
  bool staConfigured() const;
  bool staConnected() const;
  String staIp() const;
  String staHostname() const { return host_; }
  void loopSta();  // 非阻塞重连 + 状态打印

  // OTA 就绪（ArduinoOTA begin/end 由 main 推送）
  void setOtaReady(bool on) { otaReady_ = on; }
  bool otaReady() const { return otaReady_; }

  // 跟踪模式：0=BLE, 1=Classic
  int trackMode() const { return trackMode_; }
  void setTrackMode(int mode);

  // SoftAP 开着（调试/配置）时：主循环应把射频让给 WiFi，暂停阻塞式 BLE 扫描
  // 纯 STA 已连上时不让射频（空闲保活很轻），OTA 传输期由 ArduinoOTA 回调暂停 Inquiry
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
  String host_;  // mDNS / OTA 主机名（小写，无连字符歧义：garage-xxxx）
  bool apActive_ = false;
  bool serverStarted_ = false;
  int trackMode_ = TRACK_MODE_DEFAULT;
  uint32_t apQuietUntilMs_ = 0;
  bool staWanted_ = false;   // NVS 里是否已配家庭 Wi‑Fi
  bool staTrying_ = false;   // 正在连接 / 已调用 WiFi.begin
  bool mdnsOn_ = false;
  bool otaReady_ = false;  // ArduinoOTA 是否已 begin（STA 已连）
  uint32_t staNextRetryMs_ = 0;
  uint32_t staLastLogMs_ = 0;
  // 强制门户：手机连上无外网 AP 时，DNS 全指到 192.168.4.1，系统才会弹/可开配置页
  DNSServer dns_;
  bool dnsOn_ = false;
};