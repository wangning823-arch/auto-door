#pragma once
#include <Arduino.h>
#include "config_store.h"

// 公网 HTTP OTA（仅手动/指令触发）：收到 update 令后拉 version.json，
// 校验 sha256 通过才 set_boot；无定时自动检查。
// 写 flash 期间暂停 Inquiry/NFC（由回调通知 main）。

using OtaBusyFn = void (*)(bool active);

void remoteOtaBegin(ConfigStore* cfg = nullptr);
// 开机即预留 4KB 连续 8BIT 堆，OTA Update.begin 前让出——
// 运行久后 8BIT 池碎到 max8<4KB，begin 内部 malloc 必败（err=0 实锤，
// dda0 探针 maxIn=11252 但 max8=2420）
void remoteOtaHold4k();
void remoteOtaSetBusyHook(OtaBusyFn fn);
// btBusy / wifiOk 语义同 remoteCmdService
void remoteOtaService(bool btBusy, bool wifiOk);
// 串口 ota check 立刻查一次
void remoteOtaCheckNow();
bool remoteOtaActive();
const char* remoteOtaLastMsg();
