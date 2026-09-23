#pragma once
#include <Arduino.h>

// 周期把串口日志推到 VPS（HTTP 明文 /dev/logs），避免排障再接线。
// 只在蓝牙空隙发送；失败本地环形缓冲，不阻塞 loop。

void logShipBegin();
// 写入一行（自动带换行）；容量满丢最旧
void logShipf(const char* fmt, ...);
// 调试用：串口也打一份（logShipf 已含）
void logShipPrintln(const String& line);
// btBusy: Inquiry/BLE 扫描中 → 不发网络
// wifiOk: STA 已连
void logShipService(bool btBusy, bool wifiOk);
// 强制 flush（串口命令 logs flush）
void logShipFlushNow();
size_t logShipPending();
