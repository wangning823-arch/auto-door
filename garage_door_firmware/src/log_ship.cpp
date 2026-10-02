#include "log_ship.h"
#include "crash_snap.h"
#include "config.h"
#include "device_id.h"
#include "http_client.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef LOG_SHIP_URL
#define LOG_SHIP_URL "http://door.wzx.homes/dev/logs"
#endif
#ifndef LOG_SHIP_INTERVAL_MS
// 发送节奏：每 8s 一拍（20261002 用户定的：跟随探针节奏，不搞积压1s连发——
// 高频连发在 4084 残堆上反复咀嚼是 btufail 元凶之一；环里有货就一拍发一片）
#define LOG_SHIP_INTERVAL_MS 8000UL
#endif
#ifndef LOG_SHIP_TIMEOUT_MS
#define LOG_SHIP_TIMEOUT_MS 4000
#endif
// 环形缓冲约 2.5KB：够攒半分钟关键日志，不占大块
#ifndef LOG_SHIP_RING_BYTES
#define LOG_SHIP_RING_BYTES 2560
#endif
// 关键事件行独立环：EPOCH/NFC 卡/MANUAL/AUTO/RF TX。与主环物理隔离——
// 主环被积压挤爆、或将来任何清环动作都碰不到它；发货每拍先抽它。
// 5 连刷一条开关链 ≈300B（card+MANUAL+RF TX+EPOCH），1536B 保住最近 5 链。
#ifndef LOG_SHIP_CRIT_BYTES
#define LOG_SHIP_CRIT_BYTES 1536
#endif
// 单次快照上限（分片上传）：碎片堆里 8BIT max8 实测可低至 ~2.4KB，整环 2560B
// 拷进 String 会 OOM → body 变空 → 发出空 POST → 服务端静默丢弃而环已摘走
// → 内容永久丢失且每轮重复（20260927 dda0：20 次200 空包 0 落盘的死循环）。
// 1536 < 观测最小 max8(2292)，拷贝必成；成功后 1s 续发下一片
#ifndef LOG_SHIP_CHUNK
#define LOG_SHIP_CHUNK 1536
#endif
// 下限：宁可发小片也不发零片。dda0 实测 8BIT largest 最低 980，
// 96 字节片只需 4*96+256=640，仍留 340 余量。
#ifndef LOG_SHIP_CHUNK_MIN
#define LOG_SHIP_CHUNK_MIN 96
#endif

// 分配期的固定开销：flush/service 路径已改静态缓冲后，剩余堆分配主要在
// httpSubmitPost 的 HttpJob（已 nothrow）与 WiFiClient 内部。
// 仍保留 CHURN_SLACK：测量 largest 与实际发送之间，wifi/NFC 任务可能
// 已经吃掉一部分（dda0 弱网时每天 80 次 forceStaReconnect + NFC 无休止
// auto-retry 持续搅动堆）。零余量公式会让 need 恰好等于 L，
// 测量值稍一变化就拷贝失败 → 提交前 return → 一包不出网且永不自愈
// （20260928 dda0：重启后 1-2 分钟有日志，之后 38 分钟零 POST）。
#define LOG_SHIP_OVERHEAD 256
#define LOG_SHIP_CHURN_SLACK 512

// 发送失败后的重试退避：5/10/20s 指数、30s 封顶。
// 旧实现 failStreak>=3 直接 60s：一次 2 秒瞬断也被放大成 1 分钟静默，
// dda0 弱网一天几十段 >60s 中断的时长主要是这个 60s 撑起来的。
// 真离线时不会更糟——wifiOk=false 走独立分支，根本不进这里。
static uint32_t logShipFailBackoff(uint32_t streak) {
  if (streak <= 1) return 5000UL;
  if (streak == 2) return 10000UL;
  if (streak == 3) return 20000UL;
  return 30000UL;
}

// 按当前 8BIT 最大连续块给分片封顶。
// 固定 1536 在碎片堆里会 reserve 失败 → chunk=0 → 一包不发，积压永远抽不干。
//
// 一次发送期间**跨任务**同时存活多份拷贝：
//   loop 任务  : body、s_snap
//   worker 任务: HttpJob.body、req（req 还要 160B 请求头）
// 故 4*cap + OVERHEAD + CHURN_SLACK <= largest。
static size_t safeChunk() {
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t floor_need = LOG_SHIP_OVERHEAD + LOG_SHIP_CHURN_SLACK;
  if (largest <= floor_need) return 0;  // 连最小片的余量都保不住，下轮再试
  size_t cap = (largest - floor_need) / 4;
  if (cap > LOG_SHIP_CHUNK) cap = LOG_SHIP_CHUNK;
  // 20261002 放宽 256→512：旧限是为 1.5s 老空窗写的；现在窗 2.4s+排水+允许
  // 跨窗，ACTIVE 期发货速率必须追上生产（~4KB/min），否则 2560B 环被 FIFO
  // 挤爆、心跳/纪元行整段丢失（11:07 实锤 137s 无心跳）。堆真紧时公式自然缩。
  if (largest < 8000 && cap > 512) cap = 512;
  if (cap < LOG_SHIP_CHUNK_MIN) cap = LOG_SHIP_CHUNK_MIN;
  return cap;
}

// ===== 发送阻塞点诊断（打进 status，远程即可看到卡在哪一步）=====
// why: 0=未尝试 1=在飞等待 2=wifi断 3=未到点 4=safeChunk=0(堆太碎)
//      5=环空(正常) 6=body拷贝失败 7=snap拷贝失败 8=submit失败 9=已发出
// largest8/cap/pend: 上次尝试时的 8BIT 最大块、算出的分片上限、环内积压
static volatile uint8_t s_why = 0;
static volatile uint32_t s_largest8 = 0;
static volatile uint32_t s_cap = 0;
static volatile uint32_t s_attempt = 0;  // 尝试次数（>0 说明确实走到发送流程）

// ===== 日志环放 RTC noinit 段：panic 后重启不丢现场 =====
// panic 时无法执行任何用户代码（panic_abort 反汇编后结尾就是 break 1,15；
// shutdown handler 只挂在 esp_restart 上，panic 走 esp_restart_noos 绕过它），
// 所以"崩的瞬间把日志存起来"这条路走不通。唯一可行的是让日志环本身就活在
// 跨复位保留的内存里：RTC_NOINIT_ATTR → .rtc_noinit（NOLOAD，启动不清零），
// 软复位/看门狗复位后内容保留，掉电清零。
// 不能用 RTC_DATA_ATTR——那是已初始化段，启动会从 flash 重新装载=清零。
// 掉电后该段是随机值，靠 magic + 长度双重校验，不合法就当空环优雅降级。
// magic 历史：0x4C534850("LSHP") = 单环布局（无 s_critLen）；20261002 加关键环
// 后 RTC 内存布局变了——旧布局字节不能按新布局解读，bump magic 整体作废。
#define LOG_SHIP_RTC_MAGIC 0x4C534851u  // "LSHQ"

static RTC_NOINIT_ATTR char s_ring[LOG_SHIP_RING_BYTES];
static RTC_NOINIT_ATTR size_t s_len;  // 有效字节，紧凑存放
// 关键环（发货顺序：本环 → 主环）：与主环分离，主环挤旧/清环都伤不到它
static RTC_NOINIT_ATTR char s_crit[LOG_SHIP_CRIT_BYTES];
static RTC_NOINIT_ATTR size_t s_critLen;
static RTC_NOINIT_ATTR uint32_t s_rtcMagic;
static uint32_t s_nextMs = 0;
static int s_failStreak = 0;
static bool s_inFlight = false;      // 快照已提交、结果未收
static String s_snap;                 // 在飞的请求快照（失败时塞回）；仅 service 路径用
static SemaphoreHandle_t s_mtx = nullptr;  // NFC 任务写 / loop 读写
static IPAddress s_shipIp;             // 预解析缓存：flush 走 IP 直连，跳过 DNS
static bool s_shipIpOk = false;

void logShipPoke() { s_nextMs = 0; }

// ===== flush/service 静态缓冲（第一刀：去 String 堆分配）=====
// 1388 周期 rst=4 panic 指纹：loopTask phase=ls.flush + nfc.probe + http.io，
// maxblk≈3KB 时 String host/path/body/req/raw 在碎片堆上可能走库内 abort。
// logShipf 本身已是栈缓冲；这里把 flush 整条链和 service 的环拷贝改成静态块。
// body 用 RTC 段旁的普通静态即可：flush 与 service 不会并发拷环（同在 loopTask）。
#define LOG_SHIP_HOST_CAP 64
#define LOG_SHIP_PATH_CAP 128
#define LOG_SHIP_HDR_CAP 288
#define LOG_SHIP_RAW_CAP 512

static char s_hostBuf[LOG_SHIP_HOST_CAP];
static char s_pathBuf[LOG_SHIP_PATH_CAP];
static char s_hdrBuf[LOG_SHIP_HDR_CAP];
static char s_rawBuf[LOG_SHIP_RAW_CAP];
static char s_bodyBuf[LOG_SHIP_RING_BYTES];  // 环头拷贝；service 回填也用它

// RTC 段合法性：magic 对且两环长度都在界内（掉电后两者都是垃圾）
static bool rtcRingOk() {
  return s_rtcMagic == LOG_SHIP_RTC_MAGIC && s_len < LOG_SHIP_RING_BYTES &&
         s_critLen < LOG_SHIP_CRIT_BYTES;
}
static void rtcRingArm() { s_rtcMagic = LOG_SHIP_RTC_MAGIC; }

static void ringLock() {
  if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void ringUnlock() {
  if (s_mtx) xSemaphoreGive(s_mtx);
}

void logShipDiag(uint8_t* why, uint32_t* largest8, uint32_t* cap,
                 uint32_t* pend, uint32_t* attempt) {
  if (why) *why = s_why;
  if (largest8) *largest8 = s_largest8;
  if (cap) *cap = s_cap;
  if (attempt) *attempt = s_attempt;
  if (pend) {
    ringLock();
    *pend = (uint32_t)(s_len + s_critLen);
    ringUnlock();
  }
}

// ===== 关键环（发货顺序在主环之前；满则挤自己最旧，主环不受影响）=====
static void critPush(const char* s, size_t n) {
  if (n == 0) return;
  if (!rtcRingOk()) {
    s_len = 0;
    s_critLen = 0;
    rtcRingArm();
  }
  if (n >= LOG_SHIP_CRIT_BYTES) {
    s += (n - LOG_SHIP_CRIT_BYTES) + 1;
    n = LOG_SHIP_CRIT_BYTES - 1;
  }
  if (s_critLen + n >= LOG_SHIP_CRIT_BYTES) {
    size_t drop = s_critLen + n - (LOG_SHIP_CRIT_BYTES - 1);
    if (drop >= s_critLen) {
      s_critLen = 0;
    } else {
      memmove(s_crit, s_crit + drop, s_critLen - drop);
      s_critLen -= drop;
    }
  }
  memcpy(s_crit + s_critLen, s, n);
  s_critLen += n;
}

// 发送失败把快照塞回关键环队头（下拍最先重发）
static void critPrepend(const char* s, size_t n) {
  if (n == 0) return;
  if (!rtcRingOk()) {
    s_len = 0;
    s_critLen = 0;
    rtcRingArm();
  }
  if (n >= LOG_SHIP_CRIT_BYTES) {
    s += (n - (LOG_SHIP_CRIT_BYTES - 1));
    n = LOG_SHIP_CRIT_BYTES - 1;
    s_critLen = 0;
  }
  if (s_critLen + n >= LOG_SHIP_CRIT_BYTES) {
    size_t drop = s_critLen + n - (LOG_SHIP_CRIT_BYTES - 1);
    if (drop >= s_critLen) {
      s_critLen = 0;
    } else {
      memmove(s_crit, s_crit + drop, s_critLen - drop);
      s_critLen -= drop;
    }
  }
  memmove(s_crit + n, s_crit, s_critLen);
  memcpy(s_crit, s, n);
  s_critLen += n;
}

static void ringPush(const char* s, size_t n) {
  if (n == 0) return;
  // RTC 段在掉电后是随机值：先校验再动 s_len，否则垃圾长度会越界读写
  if (!rtcRingOk()) {
    s_len = 0;
    rtcRingArm();
  }
  if (n >= LOG_SHIP_RING_BYTES) {
    s += (n - LOG_SHIP_RING_BYTES) + 1;
    n = LOG_SHIP_RING_BYTES - 1;
  }
  if (s_len + n >= LOG_SHIP_RING_BYTES) {
    size_t drop = s_len + n - (LOG_SHIP_RING_BYTES - 1);
    if (drop >= s_len) {
      s_len = 0;
    } else {
      memmove(s_ring, s_ring + drop, s_len - drop);
      s_len -= drop;
    }
  }
  memcpy(s_ring + s_len, s, n);
  s_len += n;
}

void logShipBegin() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  ringLock();
  // 上一轮日志跨复位留在 RTC 里：panic/看门狗复位后重启，这里把它续传出去。
  // 掉电后 RTC 是随机值 → rtcRingOk() 不过 → 清空当新环；长度越界同样清。
  size_t prevLen = rtcRingOk() ? (s_len + s_critLen) : 0;
  if (!prevLen) {
    s_len = 0;
    s_critLen = 0;
  }
  rtcRingArm();
  // 在旧日志前插一条分隔标记：VPS 时间戳是上报时刻，不分段会把
  // "崩溃前的日志"误读成"开机后的日志"。标记进关键环队头——关键环先发货，
  // 标记必先于两环遗留内容出网。
  if (prevLen > 0) {
    char mark[96];
    int m = snprintf(mark, sizeof(mark),
                     "[LOGSHIP] ==== 以下 %u 字节为上一轮(rst=%d)遗留 ====\n",
                     (unsigned)prevLen, (int)esp_reset_reason());
    // snprintf 截断时返回的是"所需长度"而非实写长度，超界会把 mark 外的内存读出去
    if (m > 0 && m < (int)sizeof(mark)) critPrepend(mark, (size_t)m);
    Serial.printf("[LOGSHIP] resume %u bytes from prev run (rst=%d)\n",
                  (unsigned)prevLen, (int)esp_reset_reason());
  }
  ringUnlock();
  s_nextMs = millis() + (prevLen ? 1000UL : 8000UL);  // 有遗留就尽快发出去
  s_failStreak = 0;
  s_inFlight = false;
  Serial.println("[LOGSHIP] begin url=" LOG_SHIP_URL);
}

// 事件发生时间前缀（"YYYY-MM-DD HH:MM:ss "，含尾空格）。
// 只在 NTP 同步后打：未同步时 time() 是 1970 起算值，打出去比不打更误导，
// 交给 VPS 用接收时间兜底。VPS 的 _stamp_device_log 见到已带时间戳的行会跳过。
static int eventTsPrefix(char* ts, size_t cap) {
  time_t now = time(nullptr);
  if (now < 1600000000) return 0;  // 2020-09 之前 = 尚未同步
  struct tm tmv;
  if (!localtime_r(&now, &tmv)) return 0;
  if (tmv.tm_year + 1900 < 2020) return 0;
  return (int)strftime(ts, cap, "%Y-%m-%d %H:%M:%S ", &tmv);
}

void logShipPrintln(const String& line) {
  char ts[24];
  int tn = eventTsPrefix(ts, sizeof(ts));
  String t = tn > 0 ? (String(ts) + line) : line;
  t += "\n";
  ringLock();
  ringPush(t.c_str(), t.length());
  ringUnlock();
  // 环先收（VPS 必达），串口尽力而为：TX FIFO 不空闲时 println 会阻塞
  // 最多 1s 拖死 loop，无串口主机时更是永远填满
  if (Serial.availableForWrite() > 96) Serial.println(line);
}

// 行装配（logShipf / logShipCriticalf 共用尾段）：时间戳前缀 + 换行 + 入环 + 串口
static void shipEnqueue(char* buf, size_t cap, size_t len, bool critical) {
  // 事件时间戳前缀：这行日志是「什么时候发生的」，不是「什么时候传上来的」。
  // 离线 200s 后补传的积压，用接收时间会整体错位 200s。
  char ts[24];
  int tn = eventTsPrefix(ts, sizeof(ts));
  if (tn > 0) {
    if (len + (size_t)tn > cap - 2) len = cap - 2 - tn;  // 留 \n+NUL
    memmove(buf + tn, buf, len);
    memcpy(buf, ts, tn);
    len += (size_t)tn;
  }
  if (len + 1 >= cap) {
    // 截断后连 '\n' 都放不下：压掉最后一个字符也要保证环里是完整一行
    buf[cap - 2] = '\n';
    buf[cap - 1] = '\0';
    len = cap - 2;
  } else {
    buf[len] = '\n';
    buf[len + 1] = '\0';
  }
  ringLock();
  if (critical) {
    critPush(buf, len + 1);
  } else {
    ringPush(buf, len + 1);
  }
  ringUnlock();
  // 心跳/诊断必须恒发 VPS：串口只在 TX FIFO 有余量时打，阻塞风险由门控承担，
  // 环推送不受影响（1388 无串口主机，旧写法把整个 logShipf 门控掉了）。
  if (Serial.availableForWrite() > 96) {
    buf[len] = '\0';  // println 自带换行，别打两个空行
    Serial.println(buf);
  }
}

void logShipf(const char* fmt, ...) {
  // 256 而非 192：心跳行实测 188~190B，rssi/seen/open_ts 位数一涨就 ≥191。
  // 旧实现 192B 缓冲 + "截断就不进环"会把整条丢掉——1388 升级后 8s 一条的心跳
  // 只发出去 3 条（恰好都是 188~190B 的短行），带 seen=87552 的全军覆没，
  // 串口侧又因 FIFO 满不打 → 远程彻底看不到心跳。
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = (size_t)n;
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;  // 超长：拿截断结果，也好过不发
  shipEnqueue(buf, sizeof(buf), len, false);
}

void logShipCriticalf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = (size_t)n;
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  shipEnqueue(buf, sizeof(buf), len, true);
}

size_t logShipPending() {
  ringLock();
  size_t n = s_len + s_critLen;
  ringUnlock();
  return n;
}

// 解析 LOG_SHIP_URL → host/port/path（纯 char，不碰堆）
static void parseLogUrlBuf(char* host, size_t hostCap, uint16_t* port,
                           char* path, size_t pathCap) {
  if (hostCap) {
    strncpy(host, LOG_SHIP_HOST, hostCap - 1);
    host[hostCap - 1] = '\0';
  }
  if (port) *port = 80;
  if (pathCap) {
    strncpy(path, "/dev/logs", pathCap - 1);
    path[pathCap - 1] = '\0';
  }
  const char* url = LOG_SHIP_URL;
  if (strncmp(url, "http://", 7) != 0) return;
  const char* rest = url + 7;
  const char* slash = strchr(rest, '/');
  const char* hpEnd = slash ? slash : (rest + strlen(rest));
  const char* colon = (const char*)memchr(rest, ':', (size_t)(hpEnd - rest));
  if (hostCap) {
    const char* hBegin = rest;
    const char* hEnd = colon ? colon : hpEnd;
    size_t hl = (size_t)(hEnd - hBegin);
    if (hl >= hostCap) hl = hostCap - 1;
    memcpy(host, hBegin, hl);
    host[hl] = '\0';
  }
  if (port && colon) *port = (uint16_t)atoi(colon + 1);
  if (pathCap && slash) {
    strncpy(path, slash, pathCap - 1);
    path[pathCap - 1] = '\0';
  }
}

static void buildPathBuf(char* path, size_t cap) {
  if (!path || cap < 2) return;
  if (strstr(path, "id=")) return;
  size_t len = strlen(path);
  if (len + 16 >= cap) return;
  const char* sep = strchr(path, '?') ? "&" : "?";
  snprintf(path + len, cap - len, "%sid=%s", sep, deviceId());
}

// 从 HTTP 响应行取状态码："HTTP/1.1 200 OK" → 200
static int parseHttpStatusBuf(const char* raw) {
  const char* sp = strchr(raw, ' ');
  if (!sp) return -1;
  return atoi(sp + 1);
}

// 预解析日志服务器 IP：必须在网络正常、非停滞上下文调用（如 OTA 入口）。
// 之后 logShipFlushNow 全程用缓存 IP 直连，避开"黑洞期 DNS 阻塞 >5s → TWT"。
void logShipResolve() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress addr;
  esp_task_wdt_reset();
  // 先拿 DNS 锁、再撤看门狗：等锁期间看门狗仍开着，锁内每 50ms 喂一次，
  // 卡在锁上也会被 TWT 救回来而不是静默挂死。worker 与本函数并发调
  // hostByName 会踩烂框架的事件位握手（见 http_client.h），拿不到就跳过本轮，
  // 下次 flush / OTA fetch 自会重试。
  crashSnapMark("ls.dns");
  if (!httpDnsLock(HTTP_DNS_LOCK_WAIT_LOOP_MS)) {
    Serial.println("[LOGSHIP] dns lock busy -> skip resolve");
    return;
  }
  // hostByName 内部有界（IDLE 16s + DONE 15s），但 loopTask 看门狗 5s 就咬：
  // OTA/flush 入口的这次解析一卡过 5s 就 TWT 复位（dda0 实测两次 OTA 均死于此
  // 类路径）。解析期间撤监控，结束立刻补喂。
  disableLoopWDT();
  bool ok = WiFi.hostByName(LOG_SHIP_HOST, addr);
  enableLoopWDT();
  esp_task_wdt_reset();
  httpDnsUnlock();
  if (ok) {
    s_shipIp = addr;
    s_shipIpOk = true;
    Serial.printf("[LOGSHIP] resolve %s -> %s\n", LOG_SHIP_HOST,
                  addr.toString().c_str());
  } else {
    s_shipIpOk = false;
    Serial.printf("[LOGSHIP] resolve failed host=%s\n", LOG_SHIP_HOST);
  }
}

// 受控发送：200ms 一片、每片喂狗，总预算 3s；发不完返回 false（日志留环）
static bool shipSendAll(int sfd, const char* data, size_t total, uint32_t budgetMs) {
  size_t sent = 0;
  uint32_t st0 = millis();
  while (sent < total) {
    esp_task_wdt_reset();
    if (millis() - st0 > budgetMs) return false;
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sfd, &wset);
    struct timeval tv = {0, 200000};
    int r = select(sfd + 1, nullptr, &wset, nullptr, &tv);
    if (r < 0) return false;
    if (r > 0 && FD_ISSET(sfd, &wset)) {
      int n = send(sfd, data + sent, total - sent, MSG_DONTWAIT);
      if (n > 0) {
        sent += (size_t)n;
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
      }
    }
  }
  return true;
}

// 同步刷出（OTA 重启前专用）：worker 可能还有在飞，重复发一遍无害（幂等追加）
// [FLUSH] 打点用于定位 TWT：崩溃时串口最后一条 FLUSH 行 = 卡住的段
// 第一刀：整条链去 String —— host/path/body/req/raw 全走静态缓冲，
// 头与 body 分开发，不再拼成一块大 req（碎片堆上 String 拼接是 abort 源之一）。
void logShipFlushNow() {
  uint32_t t0 = millis();
  crashSnapMark("ls.flush");
  Serial.printf("[FLUSH] enter t=%u\n", (unsigned)t0);
  s_nextMs = 0;
  uint16_t port = 80;
  parseLogUrlBuf(s_hostBuf, sizeof(s_hostBuf), &port, s_pathBuf,
                 sizeof(s_pathBuf));
  buildPathBuf(s_pathBuf, sizeof(s_pathBuf));

  ringLock();
  const size_t fmax = safeChunk();
  size_t want = s_len + s_critLen;
  size_t fchunk = want > fmax ? fmax : want;
  size_t ftakeC = 0;
  if (fchunk > 0) {
    // 先拷关键环后拷主环（发货顺序 = 拷贝顺序 = 200 后的摘除顺序）
    ftakeC = s_critLen < fchunk ? s_critLen : fchunk;
    if (ftakeC > 0) memcpy(s_bodyBuf, s_crit, ftakeC);
    size_t fm = fchunk - ftakeC;
    if (fm > s_len) {
      fm = s_len;
      fchunk = ftakeC + fm;
    }
    if (fm > 0) memcpy(s_bodyBuf + ftakeC, s_ring, fm);
  }
  ringUnlock();
  if (fchunk == 0 || WiFi.status() != WL_CONNECTED) {
    Serial.printf("[FLUSH] early skip dt=%u\n", (unsigned)(millis() - t0));
    return;
  }

  // 预解析缓存 IP 直连（见 logShipResolve）：网络黑洞期 DNS 可阻塞 >5s
  // → loopTask TWT 崩溃；无缓存且 resolve 失败则跳过本次 flush，
  // 日志留在环里，网络恢复后由 logShipService 补发
  if (!s_shipIpOk) logShipResolve();
  if (!s_shipIpOk) {
    Serial.printf("[FLUSH] no ip dt=%u\n", (unsigned)(millis() - t0));
    return;
  }
  esp_task_wdt_reset();
  WiFiClient client;
  bool conn = client.connect(s_shipIp, port, LOG_SHIP_TIMEOUT_MS);
  Serial.printf("[FLUSH] conn=%d dt=%u\n", (int)conn, (unsigned)(millis() - t0));
  if (!conn) return;
  esp_task_wdt_reset();

  int hdrLen = snprintf(s_hdrBuf, sizeof(s_hdrBuf),
                        "POST %s HTTP/1.1\r\nHost: %s\r\n"
                        "User-Agent: garage-esp32\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %u\r\n"
                        "Connection: close\r\n\r\n",
                        s_pathBuf, s_hostBuf, (unsigned)fchunk);
  if (hdrLen <= 0 || hdrLen >= (int)sizeof(s_hdrBuf)) {
    client.stop();
    return;
  }

  // 头 + body 分两段 send：避免再拼一块连续 req
  // WiFiClient::write 内部是 1s select × 10 轮，网络卡顿时最坏阻塞 10s
  // ＞ TWT 5s → loopTask 崩溃。自写受控发送，总预算 3s
  int sfd = client.fd();
  bool sentOk = shipSendAll(sfd, s_hdrBuf, (size_t)hdrLen, 3000) &&
                shipSendAll(sfd, s_bodyBuf, fchunk, 3000);
  Serial.printf("[FLUSH] sent hdr=%d body=%u ok=%d dt=%u\n", hdrLen,
                (unsigned)fchunk, (int)sentOk, (unsigned)(millis() - t0));
  if (!sentOk) {
    client.stop();
    return;
  }

  uint32_t start = millis();
  size_t rawN = 0;
  s_rawBuf[0] = '\0';
  while (client.connected() || client.available()) {
    if (millis() - start > LOG_SHIP_TIMEOUT_MS) break;
    while (client.available() && rawN + 1 < sizeof(s_rawBuf)) {
      int ch = client.read();
      if (ch < 0) break;
      s_rawBuf[rawN++] = (char)ch;
    }
    s_rawBuf[rawN] = '\0';
    if (strstr(s_rawBuf, "\r\n\r\n") && !client.connected()) break;
    if (rawN + 1 >= sizeof(s_rawBuf)) break;
    delay(1);
    esp_task_wdt_reset();
  }
  client.stop();
  int code = rawN > 0 ? parseHttpStatusBuf(s_rawBuf) : -1;
  Serial.printf("[FLUSH] done code=%d dt=%u\n", code, (unsigned)(millis() - t0));
  if (code == 200) {
    ringLock();
    // 只摘已发出的一片（与拷贝同序：先关键环后主环）；新日志在尾部不受影响
    if (ftakeC > 0 && ftakeC <= s_critLen) {
      memmove(s_crit, s_crit + ftakeC, s_critLen - ftakeC);
      s_critLen -= ftakeC;
    }
    size_t fm = fchunk - ftakeC;
    if (fm > 0 && fm <= s_len) {
      memmove(s_ring, s_ring + fm, s_len - fm);
      s_len -= fm;
    }
    ringUnlock();
    s_failStreak = 0;
  }
  s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
}

void logShipService(bool btBusy, bool wifiOk) {
  // 先收结果、后过 busy 门（20261002）：结果若被 btBusy 挡在门外，s_inFlight
  // 永久悬挂 → owner 卡死不再提交 → 静默且无法自愈（1388 升级后 12min 零上报
  // 的合谋因素）。忙时只收不发，收完仍走下面的 btBusy 门。
  if (s_inFlight) {
    s_why = 1;
    int code = 0;
    if (!httpTryResult(HTTP_OWNER_LOGS, &code, nullptr)) return;
    s_inFlight = false;
    if (code == 200) {
      s_failStreak = 0;
      s_why = 9;
      // 环里还有积压 → 下一拍（8s）接着发下一片；不搞 1s 连发（见 INTERVAL 注释）。
      // 溢出保护：积压过半（ACTIVE 高产期生产>发货会把环挤爆、FIFO 丢心跳）
      // → 加速到 2s 续发；正常积压仍是 8s 一拍
      ringLock();
      size_t pend = s_len + s_critLen;
      ringUnlock();
      s_nextMs = millis() +
                 (pend > LOG_SHIP_RING_BYTES / 2 ? 2000UL : LOG_SHIP_INTERVAL_MS);
    } else {
      s_failStreak++;
      s_why = 10;  // 收到非200（网络失败/服务端拒）
      ringLock();
      critPrepend(s_snap.c_str(), s_snap.length());
      ringUnlock();
      s_nextMs = millis() + logShipFailBackoff(s_failStreak);
    }
    s_snap = "";
    return;
  }

  // 方向A：inquiry 中或刚结束保护窗 → 不发日志大包，堆留给 BTU
  if (btBusy) {
    s_why = 12;
    return;
  }

  if (!wifiOk) {
    s_why = 2;
    return;
  }
  const uint32_t now = millis();
  if ((int32_t)(now - s_nextMs) < 0) {
    s_why = 3;
    return;
  }

  uint16_t port = 80;
  parseLogUrlBuf(s_hostBuf, sizeof(s_hostBuf), &port, s_pathBuf,
                 sizeof(s_pathBuf));

  // 快照环头 ≤自适应上限，见 safeChunk()/LOG_SHIP_CHUNK 注释的空包死循环；
  // 摘环仅在拷贝成功后执行——拷贝失败时内容留环里下轮再试（旧实现先清环会丢日志）
  // 环拷贝走静态 s_bodyBuf，不再在 loopTask 上 reserve String
  const size_t s_chunkMax = safeChunk();
  s_largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s_cap = (uint32_t)s_chunkMax;
  size_t chunk = 0;
  if (s_chunkMax == 0) {
    // 堆碎片到连小片都放不下：别摘环，3s 后重试（30s 会让黑窗拖太久）
    s_why = 4;
    s_nextMs = now + 3000;
    return;
  }
  ringLock();
  if (s_critLen > 0 || s_len > 0) {
    s_attempt++;
    // 摘取顺序 = 发货顺序：先关键环后主环，关键行每拍最先出网
    size_t want = s_len + s_critLen;
    chunk = want > s_chunkMax ? s_chunkMax : want;
    if (chunk > sizeof(s_bodyBuf)) chunk = sizeof(s_bodyBuf);
    size_t takeC = s_critLen < chunk ? s_critLen : chunk;
    if (takeC > 0) {
      memcpy(s_bodyBuf, s_crit, takeC);
      memmove(s_crit, s_crit + takeC, s_critLen - takeC);
      s_critLen -= takeC;
    }
    size_t takeM = chunk - takeC;
    if (takeM > s_len) {
      takeM = s_len;
      chunk = takeC + takeM;
    }
    if (takeM > 0) {
      memcpy(s_bodyBuf + takeC, s_ring, takeM);
      memmove(s_ring, s_ring + takeM, s_len - takeM);
      s_len -= takeM;
    }
  }
  ringUnlock();

  if (chunk == 0) {
    // 环为空（正常静默）：等下一轮
    s_why = 5;
    s_nextMs = now + LOG_SHIP_INTERVAL_MS;
    return;
  }
  buildPathBuf(s_pathBuf, sizeof(s_pathBuf));
  // 提交给 worker 仍走 String（httpSubmitPost API）；从静态块构造，
  // 长度不符则回填。HttpJob 已 nothrow new，此处失败不会 terminate。
  String body;
  body.reserve(chunk);
  body.concat(s_bodyBuf, chunk);
  if (body.length() != chunk) {
    s_why = 6;  // body String 构造不完整 → 不认摘环，回填静态块
    body = "";
    ringLock();
    critPrepend(s_bodyBuf, chunk);
    ringUnlock();
    s_nextMs = now + 3000;
    return;
  }
  // 回填用的 s_snap 必须在提交前拷贝成功：环已在上面摘走，
  // 若 s_snap 拷贝 OOM 变空串，非 200 时 ringPrepend 会回填空气，日志照丢。
  s_snap = body;
  if (s_snap.length() != body.length()) {
    s_why = 7;
    s_snap = "";
    ringLock();
    critPrepend(s_bodyBuf, chunk);
    ringUnlock();
    s_nextMs = now + 3000;
    return;
  }
  if (httpSubmitPost(HTTP_OWNER_LOGS, s_hostBuf, port, s_pathBuf, body,
                     LOG_SHIP_TIMEOUT_MS)) {
    s_why = 8;  // 已入队，等 worker 发出
    s_inFlight = true;
  } else {
    // 队列满 / HttpJob 拷贝失败：塞回，稍后重试
    s_why = 11;
    s_snap = "";
    ringLock();
    critPrepend(s_bodyBuf, chunk);
    ringUnlock();
    s_nextMs = now + 3000;
  }
}
