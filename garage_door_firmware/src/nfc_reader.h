#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// PN532 NFC（I2C）：读卡 UID
// 异步：独立 FreeRTOS 任务做 I2C，主循环只收「已读到卡」事件。
// 若探测到 IRQ 线（模块拉高）→ 中断唤醒读卡；否则按 pollGap 轮询。
class NfcReader {
 public:
  // irqPin < 0：不探测 IRQ，只用任务轮询
  bool begin(int sda, int scl, int irqPin = 4);
  bool ok() const { return ok_; }

  // 同步读一帧（串口诊断 / 旧路径）；异步模式下请用 service()
  bool poll(String& uid);

  // 主循环：取出读卡事件并回调 onCard（在 loop 上下文，可安全开门）
  typedef void (*CardFn)(const String& uid);
  void setCardHandler(CardFn fn) { onCard_ = fn; }
  void service();

  // OTA 写 flash / 总线占用时暂停任务
  void setSuspended(bool on) { suspended_ = on; }
  bool suspended() const { return suspended_; }
  // OTA 前停掉片上 InList 并松总线，避免重启后 SCL 被按死
  void stopForOta();

  // 是否探测到 IRQ 线
  bool irqWired() const { return irqWired_; }
  bool asyncRunning() const { return task_ != nullptr; }

  void setAuthUid(const String& uid) { authUid_ = uid; }
  String authUid() const { return authUid_; }
  bool isAuthorized(const String& uid) const;

  String debugLine() const;
  bool forceInit();
  void setListen(bool on) { listen_ = on; }
  bool listen() const { return listen_; }
  void startListen(uint32_t listenMs = 0);  // 0=持续监听
  void setPollGapMs(uint32_t ms) { pollGapMs_ = ms; }
  uint32_t pollGapMs() const { return pollGapMs_; }
  void holdSclHigh();
  void releaseScl();
  bool deferred() const { return deferred_; }
  uint8_t autoRetryCount() const { return autoRetryCount_; }
  bool absent() const { return absent_; }
  void postponeBootInit(uint32_t delayMs);
  void kickRecover();

 private:
  bool hwInit();
  void maybeRecover();
  bool recoverBusAndResync();
  bool probePresent();
  bool detectIrqWired();
  bool lockBus(uint32_t timeoutMs = 1000);
  void unlockBus();
  void pushCard(const String& uid);
  static void taskTrampoline(void* arg);
  void taskLoop();

  bool ok_ = false;
  bool deferred_ = false;
  bool absent_ = false;
  bool listen_ = true;
  uint32_t pollGapMs_ = 350;
  bool bootInitDone_ = false;
  uint32_t bootInitAt_ = 0;
  uint8_t bootPostpones_ = 0;
  uint32_t lastAutoRetryMs_ = 0;
  uint8_t autoRetryCount_ = 0;
  uint32_t listenUntilMs_ = 0;
  int sda_ = -1, scl_ = -1, irq_ = -1;
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

  // 异步
  TaskHandle_t task_ = nullptr;
  QueueHandle_t cardQ_ = nullptr;
  SemaphoreHandle_t busMux_ = nullptr;
  volatile bool suspended_ = false;
  volatile bool irqWired_ = false;
  CardFn onCard_ = nullptr;
};
