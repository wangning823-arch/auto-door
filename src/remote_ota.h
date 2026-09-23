#pragma once
#include <Arduino.h>
#include "config_store.h"

// 公网 HTTP OTA：设备定时拉 version.json，版本不同则下载 firmware.bin
// 仅在蓝牙空隙执行；写 flash 期间暂停 Inquiry/NFC（由回调通知 main）

using OtaBusyFn = void (*)(bool active);

void remoteOtaBegin(ConfigStore* cfg = nullptr);
void remoteOtaSetBusyHook(OtaBusyFn fn);
// btBusy / wifiOk 语义同 remoteCmdService
void remoteOtaService(bool btBusy, bool wifiOk);
// 串口 ota check 立刻查一次
void remoteOtaCheckNow();
bool remoteOtaActive();
const char* remoteOtaLastMsg();
