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
  // 轮询间隔：蓝牙跟踪中可加大，减少 I2C 阻塞拖慢 Inquiry/BLE
  void setPollGapMs(uint32_t ms) { pollGapMs_ = ms; }
  uint32_t pollGapMs() const { return pollGapMs_; }
  // 强制把 SCL 推到高（插入模块前用），再 sclrelease 回上拉
  void holdSclHigh();
  void releaseScl();
  // 上电自动 init / 慢速重试状态（网页诊断用）
  bool deferred() const { return deferred_; }
  uint8_t autoRetryCount() const { return autoRetryCount_; }
  // 短超时探测：无 PN532（本机未接模块）→ 不再长超时撞 I2C
  bool absent() const { return absent_; }
  // SoftAP 配置中推迟自动 init，避免 I2C 长操作卡住 HTTP
  void postponeBootInit(uint32_t delayMs);
  // 关热点 / 立刻允许下一轮 hwInit（清 10 分钟慢速重试等待）
  void kickRecover();

 private:
  bool hwInit();
  void maybeRecover();
  bool recoverBusAndResync();
  // 返回 true=总线上像有 PN532；false=未接/无应答（短超时）
  bool probePresent();

  bool ok_ = false;
  bool deferred_ = false;
  bool absent_ = false;  // 探测无芯片：自动 init 全部跳过
  bool listen_ = true;  // 就绪后默认持续读（门应用需要）
  uint32_t pollGapMs_ = 350;
  // 上电自动 init：避免断电重启后 NFC 永久失效（原先 deferred 永不自动初始化）
  bool bootInitDone_ = false;
  uint32_t bootInitAt_ = 0;
  uint8_t bootPostpones_ = 0;  // 热点有人时最多再推迟 3 次×1.5s，之后必须 init
  uint32_t lastAutoRetryMs_ = 0;
  uint8_t autoRetryCount_ = 0;
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
  uint8_t slowAckStreak_ = 0;
  bool lastPollSlow_ = false;
  uint16_t emptyPolls_ = 0;
};
