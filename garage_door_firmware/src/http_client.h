#pragma once
#include <Arduino.h>

// 异步 HTTP：请求丢给独立任务执行，loop 只负责提交/收结果，永不同步阻塞。
// 射频仲裁：
//  - HTTP 任务发送前等蓝牙空隙（btBusy 回调），发送期间标记占用；
//  - BleTracker 见占用则推迟下一轮 inquiry（已在跑的不打断）。

enum HttpOwner {
  HTTP_OWNER_POLL = 0,
  HTTP_OWNER_LOGS = 1,
  HTTP_OWNER_STATUS = 2,
  HTTP_OWNER_COUNT = 3,
};

// 返回 true = 蓝牙（Inquiry/BLE 扫描）正在占射频
using HttpBtBusyFn = bool (*)();

// worker 等 inquiry/BLE 空隙的上限：超时仍忙则放弃本单（-13），不推迟 inquiry
// 约 1.5s：inquiry 约 2.5s / 周期 4s → 空窗约 1.5s 给 HTTP
#ifndef HTTP_BT_GAP_WAIT_MS
#define HTTP_BT_GAP_WAIT_MS 1500
#endif

// 连续网络层失败（code<0：DNS/connect/读超时）达此数 → 数据面看门狗强制重连
#ifndef HTTP_NET_FAIL_KICK
#define HTTP_NET_FAIL_KICK 4
#endif

// 提交后这么久仍未被 worker 取走（结果必丢）→ 合成失败释放该 owner
#ifndef HTTP_STUCK_UNPICKED_MS
#define HTTP_STUCK_UNPICKED_MS 300000UL
#endif
// worker 取走后超过 timeout+此值 仍无结果 → 视为丢失，合成失败释放
#ifndef HTTP_STUCK_EXTRA_MS
#define HTTP_STUCK_EXTRA_MS 60000UL
#endif

// setup 早期调用一次：建队列+任务
void httpClientBegin(HttpBtBusyFn btBusyFn);
// 提交请求；false = 队列满（调用方稍后重试）。owner 同一时刻最多一个在飞
bool httpSubmitGet(int owner, const String& host, uint16_t port,
                   const String& path, uint32_t timeoutMs);
bool httpSubmitPost(int owner, const String& host, uint16_t port,
                    const String& path, const String& body,
                    uint32_t timeoutMs);
// 非阻塞取该 owner 的结果；拿到 true（code = HTTP 状态，或 -1x 本地错误）
bool httpTryResult(int owner, int* code, String* body);
// HTTP 占用射频中（发送/等蓝牙）→ 新一轮 inquiry 让路
bool httpClientBusy();
// 本地网页正在发送大响应（web_busy 窗口内出向请求会等待）
bool httpClientWebBusy();
// httpWorker 任务栈剩余水位（字节，0=任务未起）——堆侦查诊断用
uint32_t httpClientWorkerStackHwm();
// 本地网页正在响应（loop 在发页面）→ worker 让路，等页面发完再发 VPS
void httpSetWebBusy(bool busy);
// OTA 前调用：暂停新提交并排空在飞请求（等 worker 空闲，最多 waitMs）
// 返回 true=已空闲；失败仍保持暂停，调用方必须自行 httpResume 恢复
bool httpPause(uint32_t waitMs);
// 恢复接受提交
void httpResume();

// ===== DNS 互斥 =====
// WiFi.hostByName 内部是跨任务事件位握手（WIFI_DNS_IDLE_BIT/DONE_BIT），
// 两次 waitStatusBits 的超时返回值都被丢弃（WiFiGeneric.cpp hostByName），
// IDLE 到点后照样 clearStatusBits 往下走——并发调用必然互踩 DONE 位。
// 更糟：dns_gethostbyname 的回调 arg 指向调用者栈上的 aResult，超时返回后
// 回调仍挂在 lwIP 表里，回包会写进已释放的栈帧。
// 实测后果：dda0 OTA 入口 loop 与 worker 同时解析 → 挂死 13.5 分钟零请求，
// 只能断电恢复；两台硬件固件完全相同，1388 网稳不撞这把锁所以从不出事。
// 所有 hostByName 必须先持这把锁；锁内只包 hostByName，别把 connect/读写圈进来。
// waitMs = 拿锁上限（在 loop 上下文调用时该值须 < 看门狗 5s）；
// 返回 false = 别的任务正在查 DNS，调用方应跳过本轮、稍后重试。
bool httpDnsLock(uint32_t waitMs);
// 仅在 httpDnsLock 返回 true 后调用
void httpDnsUnlock();
// 拿锁超时累计次数（status 上报 dnsbusy，远程即可判断是否在撞锁）
uint32_t httpDnsBusyCount();

// worker 等 DNS 锁的上限：loop 侧一次解析最坏 ~16s（框架 IDLE 16s/DONE 15s）
#ifndef HTTP_DNS_LOCK_WAIT_MS
#define HTTP_DNS_LOCK_WAIT_MS 20000
#endif
// loop 等 DNS 锁的上限：看门狗 5s 就咬，拿不到就跳过本轮解析
// （logShipFlushNow 留环重试 / OTA 走 fetchWithRetry 重试）
#ifndef HTTP_DNS_LOCK_WAIT_LOOP_MS
#define HTTP_DNS_LOCK_WAIT_LOOP_MS 4000
#endif
// 连续网络层失败计数（code<0 累加，HTTP 状态码清零）——数据面看门狗用
int httpClientNetFailStreak();
void httpClientResetNetFail();
