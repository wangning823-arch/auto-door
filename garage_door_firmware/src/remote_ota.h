#pragma once
#include <Arduino.h>
#include "config_store.h"

// 公网 HTTP OTA（仅手动/指令触发）：收到 update 令后拉 version.json，
// 校验 sha256 通过才 set_boot；无定时自动检查。
// 写 flash 期间暂停 Inquiry/NFC（由回调通知 main）。

using OtaBusyFn = void (*)(bool active);
// 软复位前钩子：每次 ESP.restart() 之前调用（成功/失败/BT拆栈恢复三条路径），
// 用来把 NFC 总线清到空闲，避免复位落在 I2C 事务中间把 PN532 卡死
using OtaPreResetFn = void (*)();

void remoteOtaBegin(ConfigStore* cfg = nullptr);
// 开机即预留 4KB 连续 8BIT 堆，OTA Update.begin 前让出——
// 运行久后 8BIT 池碎到 max8<4KB，begin 内部 malloc 必败（err=0 实锤，
// dda0 探针 maxIn=11252 但 max8=2420）
void remoteOtaHold4k();
// 共用气囊：这块 4KB 同时是 WiFi 碎片兜底（见 remote_ota.cpp 注释）。
// 分配失败钩子置位 → loop 调 Give 归还；堆宽裕时调 Rearm 收回。
void remoteOtaReserveGive();
void remoteOtaReserveRearm();
bool remoteOtaReserveHeld();
void remoteOtaSetBusyHook(OtaBusyFn fn);
void remoteOtaSetPreResetHook(OtaPreResetFn fn);
// btBusy / wifiOk 语义同 remoteCmdService
void remoteOtaService(bool btBusy, bool wifiOk);
// 串口 ota check 立刻查一次
void remoteOtaCheckNow();
bool remoteOtaActive();
const char* remoteOtaLastMsg();
