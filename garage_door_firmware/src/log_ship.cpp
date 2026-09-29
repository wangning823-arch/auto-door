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
#define LOG_SHIP_INTERVAL_MS 30000UL
#endif
#ifndef LOG_SHIP_TIMEOUT_MS
#define LOG_SHIP_TIMEOUT_MS 4000
#endif
// 环形缓冲约 2.5KB：够攒半分钟关键日志，不占大块
#ifndef LOG_SHIP_RING_BYTES
#define LOG_SHIP_RING_BYTES 2560
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

// 分配期的固定开销：4 份 String 各 ~16B 块头 + req 的 160B 请求头 = 256。
// 并发余量 CHURN_SLACK：测量 largest 与实际 4 次分配之间，wifi/NFC 任务可能
// 已经吃掉一部分（dda0 弱网时每天 80 次 forceStaReconnect + NFC 无休止
// auto-retry 持续搅动堆）。零余量公式 cap=(L-192)/4 会让 need 恰好等于 L，
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
#define LOG_SHIP_RTC_MAGIC 0x4C534850u  // "LSHP"

static RTC_NOINIT_ATTR char s_ring[LOG_SHIP_RING_BYTES];
static RTC_NOINIT_ATTR size_t s_len;  // 有效字节，紧凑存放
static RTC_NOINIT_ATTR uint32_t s_rtcMagic;
static uint32_t s_nextMs = 0;
static int s_failStreak = 0;
static bool s_inFlight = false;      // 快照已提交、结果未收
static String s_snap;                 // 在飞的请求快照（失败时塞回）
static SemaphoreHandle_t s_mtx = nullptr;  // NFC 任务写 / loop 读写
static IPAddress s_shipIp;             // 预解析缓存：flush 走 IP 直连，跳过 DNS
static bool s_shipIpOk = false;

// RTC 段合法性：magic 对且长度在界内（掉电后两者都是垃圾）
static bool rtcRingOk() {
  return s_rtcMagic == LOG_SHIP_RTC_MAGIC && s_len < LOG_SHIP_RING_BYTES;
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
    *pend = (uint32_t)s_len;
    ringUnlock();
  }
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

// 发送失败把快照塞回队头（新日志已在后面）
static void ringPrepend(const char* s, size_t n) {
  if (n == 0) return;
  if (!rtcRingOk()) {
    s_len = 0;
    rtcRingArm();
  }
  if (n >= LOG_SHIP_RING_BYTES) {
    s += (n - (LOG_SHIP_RING_BYTES - 1));
    n = LOG_SHIP_RING_BYTES - 1;
    s_len = 0;
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
  memmove(s_ring + n, s_ring, s_len);
  memcpy(s_ring, s, n);
  s_len += n;
}

void logShipBegin() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  ringLock();
  // 上一轮日志跨复位留在 RTC 里：panic/看门狗复位后重启，这里把它续传出去。
  // 掉电后 RTC 是随机值 → rtcRingOk() 不过 → 清空当新环；长度越界同样清。
  size_t prevLen = rtcRingOk() ? s_len : 0;
  if (!prevLen) s_len = 0;
  rtcRingArm();
  // 在旧日志前插一条分隔标记：VPS 时间戳是上报时刻，不分段会把
  // "崩溃前的日志"误读成"开机后的日志"
  if (prevLen > 0) {
    char mark[96];
    int m = snprintf(mark, sizeof(mark),
                     "[LOGSHIP] ==== 以下 %u 字节为上一轮(rst=%d)遗留 ====\n",
                     (unsigned)prevLen, (int)esp_reset_reason());
    // snprintf 截断时返回的是"所需长度"而非实写长度，超界会把 mark 外的内存读出去
    if (m > 0 && m < (int)sizeof(mark)) ringPrepend(mark, (size_t)m);
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

  // 事件时间戳前缀：这行日志是「什么时候发生的」，不是「什么时候传上来的」。
  // 离线 200s 后补传的积压，用接收时间会整体错位 200s。
  char ts[24];
  int tn = eventTsPrefix(ts, sizeof(ts));
  if (tn > 0) {
    if (len + (size_t)tn > sizeof(buf) - 2) len = sizeof(buf) - 2 - tn;  // 留 \n+NUL
    memmove(buf + tn, buf, len);
    memcpy(buf, ts, tn);
    len += (size_t)tn;
  }

  if (len + 1 >= sizeof(buf)) {
    // 截断后连 '\n' 都放不下：压掉最后一个字符也要保证环里是完整一行
    buf[sizeof(buf) - 2] = '\n';
    buf[sizeof(buf) - 1] = '\0';
    len = sizeof(buf) - 2;
  } else {
    buf[len] = '\n';
    buf[len + 1] = '\0';
  }
  ringLock();
  ringPush(buf, len + 1);
  ringUnlock();
  // 心跳/诊断必须恒发 VPS：串口只在 TX FIFO 有余量时打，阻塞风险由门控承担，
  // 环推送不受影响（1388 无串口主机，旧写法把整个 logShipf 门控掉了）。
  if (Serial.availableForWrite() > 96) {
    buf[len] = '\0';  // println 自带换行，别打两个空行
    Serial.println(buf);
  }
}

size_t logShipPending() {
  ringLock();
  size_t n = s_len;
  ringUnlock();
  return n;
}

// 解析 LOG_SHIP_URL → host/port/path
static void parseLogUrl(String* host, uint16_t* port, String* path) {
  *host = LOG_SHIP_HOST;
  *port = 80;
  *path = "/dev/logs";
  String url = LOG_SHIP_URL;
  if (!url.startsWith("http://")) return;
  String rest = url.substring(7);
  int slash = rest.indexOf('/');
  String hp = slash >= 0 ? rest.substring(0, slash) : rest;
  *path = slash >= 0 ? rest.substring(slash) : String("/dev/logs");
  int c = hp.indexOf(':');
  if (c >= 0) {
    *host = hp.substring(0, c);
    *port = (uint16_t)atoi(hp.substring(c + 1).c_str());
  } else {
    *host = hp;
  }
}

static void buildPath(String* path) {
  if (path->indexOf("id=") < 0) {
    *path += (path->indexOf('?') >= 0 ? '&' : '?');
    *path += "id=";
    *path += deviceId();
  }
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

// 同步刷出（OTA 重启前专用）：worker 可能还有在飞，重复发一遍无害（幂等追加）
// [FLUSH] 打点用于定位 TWT：崩溃时串口最后一条 FLUSH 行 = 卡住的段
void logShipFlushNow() {
  uint32_t t0 = millis();
  crashSnapMark("ls.flush");
  Serial.printf("[FLUSH] enter t=%u\n", (unsigned)t0);
  s_nextMs = 0;
  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);
  buildPath(&path);

  ringLock();
  String body;
  // 同样分片 + 自适应上限：整环 2560B 或固定 1536 在碎片堆 OOM 会 early-skip，
  // OTA 重启前一段日志全丢
  const size_t fmax = safeChunk();
  size_t fchunk = s_len > fmax ? fmax : s_len;
  if (fchunk > 0) {
    body.reserve(fchunk);
    body.concat(s_ring, fchunk);
    if (body.length() != fchunk) fchunk = 0;
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
  String req;
  req.reserve(160 + body.length());
  req += "POST ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: garage-esp32\r\nContent-Type: text/plain\r\n";
  req += "Content-Length: ";
  req += String((unsigned)body.length());
  req += "\r\nConnection: close\r\n\r\n";
  req += body;
  // WiFiClient::write 内部是 1s select × 10 轮，网络卡顿时最坏阻塞 10s
  // ＞ TWT 5s → loopTask 崩溃。自写受控发送：200ms 一片、每片喂狗，
  // 总预算 3s；发不完就放弃（日志留在环里，恢复后补发）
  {
    const char* p = req.c_str();
    size_t total = req.length();
    size_t sent = 0;
    uint32_t st0 = millis();
    int sfd = client.fd();
    while (sent < total) {
      esp_task_wdt_reset();
      if (millis() - st0 > 3000) break;
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET(sfd, &wset);
      struct timeval tv = {0, 200000};
      int r = select(sfd + 1, nullptr, &wset, nullptr, &tv);
      if (r < 0) break;
      if (r > 0 && FD_ISSET(sfd, &wset)) {
        int n = send(sfd, p + sent, total - sent, MSG_DONTWAIT);
        if (n > 0) {
          sent += (size_t)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          break;
        }
      }
    }
    Serial.printf("[FLUSH] sent %u/%u dt=%u\n", (unsigned)sent,
                  (unsigned)total, (unsigned)(millis() - t0));
    if (sent < total) {
      client.stop();
      return;
    }
  }
  uint32_t start = millis();
  String raw;
  while (client.connected() || client.available()) {
    if (millis() - start > LOG_SHIP_TIMEOUT_MS) break;
    while (client.available()) raw += (char)client.read();
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    delay(1);
    esp_task_wdt_reset();
    if (raw.length() > 512) break;
  }
  client.stop();
  int sp1 = raw.indexOf(' ');
  int sp2 = raw.indexOf(' ', sp1 + 1);
  int code = (sp1 >= 0 && sp2 >= 0) ? raw.substring(sp1 + 1, sp2).toInt() : -1;
  Serial.printf("[FLUSH] done code=%d dt=%u\n", code, (unsigned)(millis() - t0));
  if (code == 200) {
    ringLock();
    // 只摘已发出的环头一片；新日志在环尾不受影响
    if (fchunk <= s_len) {
      memmove(s_ring, s_ring + fchunk, s_len - fchunk);
      s_len -= fchunk;
    }
    ringUnlock();
    s_failStreak = 0;
  }
  s_nextMs = millis() + LOG_SHIP_INTERVAL_MS;
}

void logShipService(bool btBusy, bool wifiOk) {
  (void)btBusy;  // 射频仲裁在 http_client worker

  // 收结果：成功=快照已发走（ring 提交时已摘掉）；失败=塞回队头重试
  if (s_inFlight) {
    s_why = 1;
    int code = 0;
    if (!httpTryResult(HTTP_OWNER_LOGS, &code, nullptr)) return;
    s_inFlight = false;
    if (code == 200) {
      s_failStreak = 0;
      s_why = 9;
      // 环里还有积压 → 1s 后接着发下一片；发干净才回 30s 周期
      ringLock();
      size_t left = s_len;
      ringUnlock();
      s_nextMs = millis() + (left > 0 ? 1000UL : LOG_SHIP_INTERVAL_MS);
    } else {
      s_failStreak++;
      s_why = 10;  // 收到非200（网络失败/服务端拒）
      ringLock();
      ringPrepend(s_snap.c_str(), s_snap.length());
      ringUnlock();
      s_nextMs = millis() + logShipFailBackoff(s_failStreak);
    }
    s_snap = "";
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

  String host, path;
  uint16_t port = 80;
  parseLogUrl(&host, &port, &path);

  // 快照环头 ≤自适应上限，见 safeChunk()/LOG_SHIP_CHUNK 注释的空包死循环；
  // 摘环仅在拷贝成功后执行——OOM 时内容留环里下轮再试（旧实现先清环会丢日志）
  const size_t s_chunkMax = safeChunk();
  s_largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s_cap = (uint32_t)s_chunkMax;
  String body;
  size_t chunk = 0;
  if (s_chunkMax == 0) {
    // 堆碎片到连小片都放不下：别摘环，3s 后重试（30s 会让黑窗拖太久）
    s_why = 4;
    s_nextMs = now + 3000;
    return;
  }
  ringLock();
  if (s_len > 0) {
    s_attempt++;
    chunk = s_len > s_chunkMax ? s_chunkMax : s_len;
    body.reserve(chunk);
    body.concat(s_ring, chunk);
    if (body.length() == chunk) {
      memmove(s_ring, s_ring + chunk, s_len - chunk);
      s_len -= chunk;
    } else {
      s_why = 6;  // body 拷贝 OOM → 不摘环，3s 重试
      chunk = 0;
    }
  }
  ringUnlock();

  if (chunk == 0) {
    // 环为空（正常静默）或 reserve 失败（留环重试）：都等下一轮
    if (s_why != 6) s_why = 5;
    s_nextMs = now + LOG_SHIP_INTERVAL_MS;
    return;
  }
  buildPath(&path);
  // 回填用的 s_snap 必须在提交前拷贝成功：环已在上面摘走，
  // 若 s_snap 拷贝 OOM 变空串，非 200 时 ringPrepend 会回填空气，日志照丢。
  s_snap = body;
  if (s_snap.length() != body.length()) {
    s_why = 7;
    s_snap = "";
    ringLock();
    ringPrepend(body.c_str(), body.length());
    ringUnlock();
    s_nextMs = now + 3000;
    return;
  }
  if (httpSubmitPost(HTTP_OWNER_LOGS, host, port, path, body,
                     LOG_SHIP_TIMEOUT_MS)) {
    s_why = 8;  // 已入队，等 worker 发出
    s_inFlight = true;
  } else {
    // 队列满 / HttpJob 拷贝失败：塞回，稍后重试
    s_why = 11;
    s_snap = "";
    ringLock();
    ringPrepend(body.c_str(), body.length());
    ringUnlock();
    s_nextMs = now + 3000;
  }
}
