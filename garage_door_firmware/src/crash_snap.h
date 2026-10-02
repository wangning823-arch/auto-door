#pragma once
#include <Arduino.h>

// ===== 崩溃现场跨复位保留 =====
// panic 时无法执行任何用户代码（panic_abort 反汇编确认结尾就是 break 1,15；
// shutdown handler 只挂在 esp_restart 上，而 panic 走 esp_restart_noos 绕过它），
// 所以"崩溃瞬间抓栈"这条路在本框架下走不通。
// 替代方案：周期性把各任务当前调用栈快照进 RTC noinit 段（软复位后保留、
// 掉电清零），异常复位后开机随 [BOOT] 上报；loop 周期采样 250ms（20261002 提频）。
//
// 快照抓的是"崩溃前 ≤250ms 的栈"，不是崩溃瞬间的栈——配合 phase 标记足以定位。

// setup 早期调用一次（可省：magic 校验在上报时做）
void crashSnapBegin();
// 抓当前任务的调用栈 + 任务名写入 RTC（loop/nfc/httpWorker 各自周期调用）
void crashSnapCapture();
// 粗粒度阶段标记：写入当前任务槽位，崩溃后能看出"最后在干哪一步"
void crashSnapMark(const char* phase);
// 开机调用：仅异常复位（PANIC/WDT/BROWNOUT）时把各任务快照上报到日志环
void crashSnapReport(int rst);
