#include "crash_snap.h"
#include "log_ship.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_debug_helpers.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// 快照槽位数：loop / nfc / httpWorker 各占一个，够用；多了也是浪费 RTC
#define CRASH_SNAP_SLOTS 8
#define CRASH_SNAP_PCS 8
#define CRASH_TASK_NAME 16
#define CRASH_PHASE_LEN 24
#define CRASH_SNAP_MAGIC 0x43525348u  // "CRSH"

// RTC noinit 段：软复位（panic/WDT/软件重启）后保留，掉电清零。
// 必须用 RTC_NOINIT_ATTR（NOLOAD，启动不清）；RTC_DATA_ATTR 是已初始化段、
// 启动会从 flash 重载 = 清零，不能用。掉电后内容是随机值，靠 magic 挡。
struct CrashSlot {
  uint32_t magic;
  uint32_t uptimeMs;
  uint8_t pcN;
  uint8_t _rsv[3];
  char task[CRASH_TASK_NAME];
  char phase[CRASH_PHASE_LEN];
  uint32_t pc[CRASH_SNAP_PCS];
};

static RTC_NOINIT_ATTR CrashSlot s_slot[CRASH_SNAP_SLOTS];
// 待填槽位轮转（找不到同名且没有空槽时用）
static uint8_t s_rr = 0;

// 字符串在 buf[0..cap) 内有 NUL 终结且全为可打印 ASCII。
// 必须逐字节设上限——RTC 掉电后是无 NUL 的垃圾，无界扫描会读出数组外。
static bool cStrOk(const char* buf, int cap) {
  for (int i = 0; i < cap; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (c == 0) return true;  // 空串也是合法的（phase 允许未打点）
    if (c < 0x20 || c > 0x7e) return false;  // 非可打印 ASCII 即垃圾
  }
  return false;  // 满 cap 都没 NUL → 越界风险，判无效
}
static bool slotOk(const CrashSlot* s) {
  if (s->magic != CRASH_SNAP_MAGIC || s->pcN > CRASH_SNAP_PCS) return false;
  if (s->task[0] == 0) return false;  // 任务名必须有，否则上报没法归因
  return cStrOk(s->task, CRASH_TASK_NAME) && cStrOk(s->phase, CRASH_PHASE_LEN);
}

void crashSnapBegin() {
  // 不清槽位：内容由 magic 自校验，等 crashSnapReport 读一次即可。
  // 走关键环：boot 早期主环拥挤/截断时这行也必须到 VPS（确认新固件带此功能）
  logShipCriticalf("[CRASH] snap ready (rtc noinit, %u bytes)",
                   (unsigned)sizeof(s_slot));
}

static CrashSlot* pickSlot(const char* name) {
  CrashSlot* empty = nullptr;
  for (int i = 0; i < CRASH_SNAP_SLOTS; i++) {
    if (!slotOk(&s_slot[i])) {
      if (!empty) empty = &s_slot[i];
      continue;
    }
    if (strncmp(s_slot[i].task, name, CRASH_TASK_NAME - 1) == 0) {
      return &s_slot[i];  // 同名任务复用同一槽
    }
  }
  if (empty) return empty;
  CrashSlot* s = &s_slot[s_rr];  // 满了就轮转覆盖最老的
  s_rr = (uint8_t)((s_rr + 1) % CRASH_SNAP_SLOTS);
  return s;
}

void crashSnapCapture() {
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  const char* name = h ? pcTaskGetTaskName(h) : "?";
  CrashSlot* s = pickSlot(name);
  if (!s) return;

  // 槽位复用/首次填充时 phase 是上一轮残留的垃圾，必须清掉，
  // 否则崩溃上报会把别的任务的阶段安到这个任务头上
  bool fresh = !slotOk(s) || strncmp(s->task, name, CRASH_TASK_NAME - 1) != 0;
  if (fresh) s->phase[0] = 0;

  // esp_backtrace_get_start 会把寄存器窗口刷到栈上再取首帧，随后逐帧上溯。
  // 这是 esp_backtrace_print 的同款路径，在任务上下文里调用是安全的。
  esp_backtrace_frame_t fr;
  memset(&fr, 0, sizeof(fr));
  esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
  uint8_t n = 0;
  while (n < CRASH_SNAP_PCS) {
    if (fr.pc == 0) break;
    s->pc[n++] = fr.pc;
    if (fr.next_pc == 0) break;  // 已到栈顶，再取就是垃圾
    if (!esp_backtrace_get_next_frame(&fr)) break;
    if (fr.pc == 0) break;
  }

  s->uptimeMs = millis();
  s->pcN = n;
  strlcpy(s->task, name, CRASH_TASK_NAME);
  s->magic = CRASH_SNAP_MAGIC;  // 最后写：写到一半掉电/复位则 magic 不对，整槽作废
}

void crashSnapMark(const char* phase) {
  if (!phase) return;
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  const char* name = h ? pcTaskGetTaskName(h) : "?";
  CrashSlot* s = pickSlot(name);
  if (!s) return;
  // 槽位可能是轮转来的旧槽（magic 有效但 task 是别人的），此时只更新 phase 会
  // 把 A 任务的阶段记到 B 头上 → 上报时结论反了。task 不匹配就整槽重置。
  bool mismatch = !slotOk(s) || strncmp(s->task, name, CRASH_TASK_NAME - 1) != 0;
  strlcpy(s->phase, phase, CRASH_PHASE_LEN);
  s->uptimeMs = millis();
  if (mismatch) {
    s->pcN = 0;
    strlcpy(s->task, name, CRASH_TASK_NAME);
  }
  s->magic = CRASH_SNAP_MAGIC;
  // mark 即采样：mark 点都在函数内（ls.flush/nfc.probe/http.io...），此时栈是
  // 深的。周期采样（哪怕 250ms）总落在任务空闲骨架上——1341/1824 两次崩溃的
  // PC 全是同组骨架地址就是证据。嵌进来后每个阶段点留一份函数级深栈，
  // 崩溃时最新一份 mark 快照 = "最后在干哪一步"的完整调用链。
  crashSnapCapture();
}

void crashSnapReport(int rst) {
  // 只在异常复位时上报：正常上电/软件重启时 RTC 要么是垃圾要么是上轮残留，
  // 报了只会污染日志。1=POWERON 2=EXT 3=SW 都不报，8=DEEPSLEEP 不用。
  if (rst != 4 && rst != 5 && rst != 6 && rst != 7 && rst != 9) return;

  int shown = 0;
  for (int i = 0; i < CRASH_SNAP_SLOTS; i++) {
    CrashSlot* s = &s_slot[i];
    if (!slotOk(s)) continue;
    shown++;
    // 关键环：崩溃现场行开机早期必达——12:14 panic 后 task=/pc= 行没到 VPS，
    // 归因断线索（无 USB 只能靠这些行）
    logShipCriticalf("[CRASH] task=%s phase=%s up=%ums rst=%d", s->task,
                     s->phase[0] ? s->phase : "-", (unsigned)s->uptimeMs, rst);
    if (s->pcN == 0) {
      logShipCriticalf("[CRASH] %s (no pc, mark only)", s->task);
      continue;
    }
    // 8 个 PC 约 90 字节，logShipf 缓冲 192，一行放得下
    char line[160];
    int off = snprintf(line, sizeof(line), "[CRASH] %s pc:", s->task);
    for (uint8_t k = 0; k < s->pcN && off < (int)sizeof(line) - 12; k++) {
      off += snprintf(line + off, sizeof(line) - (size_t)off, " %08x",
                      (unsigned)s->pc[k]);
    }
    logShipCriticalf("%s", line);
  }
  if (!shown) {
    logShipCriticalf("[CRASH] rst=%d but no valid snapshot", rst);
  }
}
