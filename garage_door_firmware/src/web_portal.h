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

// 分片直写器（实现在 web_portal.cpp）：首页边生成边发，不在碎片堆上整体拼 String
struct PageW;

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
  // HTTP 里只打标记；真正 stopAp 放到 loop，避免在回调里改 WiFi 导致复位
  void requestStopAp() { stopApPending_ = true; }

  // 家庭路由 STA（网页保存后 / 上电自动连；用于 espota 无线烧录）
  void startStaFromStore();
  void stopSta();
  bool staConfigured() const;
  bool staConnected() const;
  String staIp() const;
  // 数据面看门狗：sta=1 但网络层连续失败（僵尸关联/IP 黑洞）→ 主动断开重连
  void forceStaReconnect();
  // 设备名（仅日志/OTA 协议元数据；无 mDNS，访问一律用 STA IP）
  String staHostname() const { return host_; }
  void loopSta();  // 非阻塞重连 + 状态打印
  // STA 拿到 IP 后确保 HTTP server 已 begin（纯 STA 模式）
  void ensureHttpIfSta();

  // 桌面 espota(ArduinoOTA) 就绪标志——已随 ArduinoOTA 移除，恒为 false；
  // 升级一律走 VPS 在线 OTA，此标志仅供 /ota JSON 如实上报
  void setOtaReady(bool on) { otaReady_ = on; }
  bool otaReady() const { return otaReady_; }

  // 本地网页 80 端口开关（配置迁 VPS 控制台后远程 web off 下线；不写 NVS，
  // 由调用方 saveWebUi；startAp 恢复路径不受限——救砖入口永远可用）
  void setUiEnabled(bool on);
  bool uiEnabled() const { return uiEnabled_; }

  // 跟踪模式：0=BLE, 1=Classic
  int trackMode() const { return trackMode_; }
  void setTrackMode(int mode);

  // SoftAP 开着（调试/配置）时：主循环应把射频让给 WiFi，暂停阻塞式 BLE 扫描
  // 纯 STA 已连上时不让射频（空闲保活很轻），VPS OTA 传输期由 busy hook 暂停 Inquiry
  bool wifiRfPriority() const { return apActive_; }
  // SoftAP 启动后的静默窗口是否仍未结束
  bool rfQuietActive() const;

 private:
  void setupRoutes();
  // 流式输出首页：全程只用栈上小缓冲，避免碎片堆(maxblk≈11KB)上拼 3~9KB 大 String
  void pageHtml(PageW& w) const;
  // 网页重响应期间暂停蓝牙 inquiry（复用 OTA 让路模式），发完恢复
  void webPauseBt();
  void webResumeBt();

  ConfigStore* store_ = nullptr;
  BleTracker* bt_ = nullptr;
  DoorFsm* door_ = nullptr;
  BleScanTool* ble_ = nullptr;
  NfcReader* nfc_ = nullptr;
  String apSsid_;
  String host_;  // 设备名（OTA 协议元数据/日志；无 mDNS）
  bool apActive_ = false;
  bool stopApPending_ = false;
  bool serverStarted_ = false;
  bool uiEnabled_ = true;  // 本地网页开关（NVS web_ui）
  int trackMode_ = TRACK_MODE_DEFAULT;
  // 跟踪模式切换后延时重启：运行中无法卸载已起的 BT 栈，重启才能真正二选一
  bool modeRebootPending_ = false;
  uint32_t modeRebootAtMs_ = 0;
  uint32_t apQuietUntilMs_ = 0;
  bool staWanted_ = false;   // NVS 里是否已配家庭 Wi‑Fi
  bool staTrying_ = false;   // 正在连接 / 已调用 WiFi.begin
  bool otaReady_ = false;  // 桌面 espota 已移除，恒 false
  uint32_t staNextRetryMs_ = 0;
  uint32_t staLastLogMs_ = 0;
  // 强制门户：手机连上无外网 AP 时，DNS 全指到 192.168.4.1，系统才会弹/可开配置页
  DNSServer dns_;
  bool dnsOn_ = false;
};