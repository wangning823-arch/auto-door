#pragma once
#include <Arduino.h>

// BLE 配对 + IRK：仅配对窗口内可绑定；密码=手机配对时输入的 6 位 PIN
class BleBond {
 public:
  void begin();
  // 只置位，真正开窗/广播在 service() 里做（避免 HTTP 回调里打断 SoftAP）
  void requestOpenPairing(uint32_t ms = 90000);
  void openPairingWindow(uint32_t ms = 90000);
  void closePairingWindow(const char* why);
  bool pairingOpen() const;
  void service();

  bool matchesAddr(const String& addrColon) const;
  bool hasIrk() const { return hasIrk_; }
  String identityMac() const { return identity_; }
  bool savePeerIdKey(const uint8_t* irk16, const uint8_t* identity6);
  void clearBond(const char* why);

  String pairingPin() const { return pin_; }
  void setPairingPin(const String& pin);
  void applyBleSecurity();
  uint32_t staticPasskey() const;
  bool hasPasskey() const;
  bool allowSmp() const { return pairingOpen(); }
  // 配对成功后从系统 bond 表抠 IRK/身份地址
  bool trySaveFromSystemBond(const uint8_t* peerAddr6);
  void requestDelayedClose(const char* why, uint32_t ms = 2000);
  void notePeer(const uint8_t* addr6);  // 回调里只记地址，service 再处理

 private:
  bool hasIrk_ = false;
  uint8_t irk_[16] = {0};
  String identity_;
  bool pairWin_ = false;
  bool pairWinSticky_ = false;
  uint32_t pairWinEndMs_ = 0;
  String pin_;
  bool pendingOpen_ = false;
  uint32_t pendingOpenMs_ = 0;
  uint32_t pendingOpenAt_ = 0;
  bool pendingClose_ = false;
  uint32_t pendingCloseAt_ = 0;
  String pendingCloseWhy_;
  uint8_t pendingPeer_[6] = {0};
  bool hasPendingPeer_ = false;

  void loadFromStore();
  void startAdv();
  void stopAdv();
  void setAdvConnectable(bool connectable);
  void disconnectAll();
  void clearSystemBonds();
  void doOpen(uint32_t ms);
};

extern BleBond gBleBond;
