#pragma once
#include <Arduino.h>

// PN532 NFC（I2C）：读卡 UID
// 初始化走完整 PN532 命令；读卡失败时自动总线恢复，避免 1~2 分钟后挂死
class NfcReader {
 public:
  bool begin(int sda, int scl);
  bool ok() const { return ok_; }

  // 读卡：就绪后默认持续轮询；读到卡返回 true
  bool poll(String& uid);

  void setAuthUid(const String& uid) { authUid_ = uid; }
  String authUid() const { return authUid_; }
  bool isAuthorized(const String& uid) const;

  String debugLine() const;
  bool forceInit();
  void setListen(bool on) { listen_ = on; }
  bool listen() const { return listen_; }
  void startListen(uint32_t listenMs = 0);  // 0=持续监听
  // 强制把 SCL 推到高（插入模块前用），再 sclrelease 回上拉
  void holdSclHigh();
  void releaseScl();

 private:
  bool hwInit();
  void maybeRecover();
  bool recoverBusAndResync();

  bool ok_ = false;
  bool deferred_ = false;
  bool listen_ = true;  // 就绪后默认持续读（门应用需要）
  uint32_t listenUntilMs_ = 0;
  int sda_ = -1, scl_ = -1;
  String authUid_;
  String lastUid_;
  uint32_t lastReadMs_ = 0;
  uint16_t failStreak_ = 0;
  uint32_t lastOkMs_ = 0;
  uint32_t lastRecoverMs_ = 0;
  uint32_t nextPollMs_ = 0;
  uint32_t lastResyncMs_ = 0;
  uint32_t lastFieldMs_ = 0;
};
