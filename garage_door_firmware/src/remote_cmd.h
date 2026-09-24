#pragma once
#include <Arduino.h>
#include "config_store.h"

// 远程令：仅在蓝牙空隙访问 WiFi（单射频时间片）
// 收到 {"cmd":"open"} 等 → 通过回调走与 TRIG 相同的 MIAO 动作

using RemoteCmdFn = void (*)(const char* cmd);
// TLS/大块分配前的堆回收（如清 BLE 广播表）
using RemoteMemTrimFn = void (*)();

void remoteCmdBegin(ConfigStore* cfg = nullptr);
void remoteCmdSetHandler(RemoteCmdFn fn);
void remoteCmdSetMemTrim(RemoteMemTrimFn fn);
// btBusy: Inquiry/BLE 扫描进行中 → 必须 return（蓝牙优先）
// wifiOk: STA 已连接
void remoteCmdService(bool btBusy, bool wifiOk);
bool remoteCmdEnabled();
void remoteCmdSetEnabled(bool on);
