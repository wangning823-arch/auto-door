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
// 本地网页正在响应（loop 在发页面）→ worker 让路，等页面发完再发 VPS
void httpSetWebBusy(bool busy);
// OTA 前调用：暂停新提交并排空在飞请求（等 worker 空闲，最多 waitMs）
// 返回 true=已空闲；失败也会保持暂停，调用方无需重试
bool httpPause(uint32_t waitMs);
// 恢复接受提交
void httpResume();
