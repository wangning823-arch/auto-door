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
// 堆从 thin 恢复后立刻尝试补发（s_nextMs=0）
void logShipPoke();
// 同步 flush：立刻 HTTP POST（OTA 重启前 / 串口 logs flush）
void logShipFlushNow();
// 预解析日志服务器 IP 并缓存（OTA 开始时网络正常时调用）：
// 下载停滞期网络黑洞中 DNS 可阻塞 >5s → TWT 崩溃，flush 必须走缓存 IP 直连
void logShipResolve();
size_t logShipPending();
// 发送阻塞点诊断：why/largest8/cap/pend/attempt
// why: 0=未尝试 1=在飞 2=wifi断 3=未到点 4=safeChunk=0 5=环空
//      6=body拷贝失败 7=snap拷贝失败 8=已入队 9=已发出 10=非200 11=submit失败
void logShipDiag(uint8_t* why, uint32_t* largest8, uint32_t* cap,
                 uint32_t* pend, uint32_t* attempt);
