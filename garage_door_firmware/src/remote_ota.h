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

// 共用气囊容量：必须 ≥ BTU inquiry 的 4112B（含 malloc 头余量），
// 同时仍覆盖 WiFi esf_buf 2308B 与 OTA Update.begin 的 ~4KB。
// 旧值 4096 盖不住 4112 → BTU 失败时钩子 even 不会请求归还（见 main.cpp）。
// 20260930 1388 实测：4352 归还后 max8 只有 ~4340，立刻被 WiFi/BTU 抢吃，
// 稳态仍回落 4084，BTU 4112 继续 fail → 抬到 12KB，Give 后留出连续余量。
#ifndef OTA_RESERVE_SIZE
#define OTA_RESERVE_SIZE 12288
#endif
// 收回门槛：largest8 ≥ 气囊 + 2KB 余量（跟踪期已禁止 rearm，此处主要兜 OTA 路径）
#ifndef OTA_RESERVE_REARM_MIN
#define OTA_RESERVE_REARM_MIN (OTA_RESERVE_SIZE + 2048)
#endif

void remoteOtaBegin(ConfigStore* cfg = nullptr);
// 开机即预留 OTA_RESERVE_SIZE 连续 8BIT 堆，OTA Update.begin 前让出——
// 运行久后 8BIT 池碎到 max8<气囊，begin 内部 malloc 必败（err=0 实锤，
// dda0 探针 maxIn=11252 但 max8=2420）
void remoteOtaHold4k();
// 共用气囊：这块内存同时是 WiFi/BTU 碎片兜底（见 remote_ota.cpp 注释）。
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
