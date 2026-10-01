#include "http_client.h"
#include "crash_snap.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <atomic>
#include <esp_task_wdt.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// 单工作任务：所有 HTTP 在这里串行执行，loop 永不阻塞。
// 射频仲裁：发送前等蓝牙空隙（有上限），发送中置 radioBusy 给 inquiry 让路。

#ifndef HTTP_TASK_STACK
#define HTTP_TASK_STACK 6144
#endif
#ifndef HTTP_TASK_PRIO
#define HTTP_TASK_PRIO 1
#endif
// 等蓝牙让路的上限：超时就直接发（防饿死）
#ifndef HTTP_BT_WAIT_MAX_MS
#define HTTP_BT_WAIT_MAX_MS 4000
#endif
// 等本地网页发完的上限：页面最坏 ~10s，留余量；超时强制发（防 VPS 饿死）
#ifndef HTTP_WEB_WAIT_MAX_MS
#define HTTP_WEB_WAIT_MAX_MS 15000
#endif

// DNS 锁等待上限见 http_client.h（HTTP_DNS_LOCK_WAIT_MS / _LOOP_MS）

struct HttpJob {
  int owner;
  uint32_t gen;  // 提交代次：owner 被在飞看门狗判死后，旧结果按代次丢弃
  bool isPost;
  String host;
  uint16_t port;
  String path;
  String body;
  uint32_t timeoutMs;
};

struct HttpSlot {
  std::atomic<bool> ready{false};
  int code = 0;
  String body;
  uint32_t gen = 0;        // 最近一次提交的代次
  uint32_t submitMs = 0;   // 提交时刻（在飞看门狗）
  uint32_t startMs = 0;    // worker 取走开始执行的时刻；0=还没被取走
  uint32_t timeoutMs = 0;
};

static QueueHandle_t s_jobs = nullptr;
static HttpSlot s_slots[HTTP_OWNER_COUNT];
static std::atomic<bool> s_ownerBusy[HTTP_OWNER_COUNT];
static std::atomic<int> s_netFail{0};      // 连续网络层失败（code<0）
static std::atomic<bool> s_radioBusy{false};
static std::atomic<bool> s_webBusy{false};   // 本地网页响应中
static std::atomic<bool> s_paused{false};    // OTA 排空期：拒新提交
static std::atomic<bool> s_workerBusy{false};  // worker 正在处理一个 job
static std::atomic<bool> s_nfcBusy{false};    // NFC probe/hwInit：禁出向 HTTP
static HttpBtBusyFn s_btBusy = nullptr;

void httpSetNfcBusy(bool busy) { s_nfcBusy.store(busy); }
bool httpNfcBusy() { return s_nfcBusy.load(); }

// ===== worker 静态 IO 缓冲（仅单工作任务使用）=====
// 1388 rst=4 指纹 http.io：碎片堆上 String req/raw 的 reserve/+= 可能库内 abort。
// 与 log_ship 同款：头/响应走静态块，body 单独 write，失败直接 return。
#define HTTP_HDR_CAP 320
#define HTTP_RAW_CAP 2048
static char s_reqHdr[HTTP_HDR_CAP];
static char s_rawBuf[HTTP_RAW_CAP];

// ===== DNS 互斥（见 http_client.h 注释）=====
// 两个任务并发调 hostByName 会把框架的事件位握手踩烂且无法自愈，
// 故所有 hostByName 都先过这里。取不到锁 = 别人正在查，调用方跳过本轮。
static SemaphoreHandle_t s_dnsMtx = nullptr;
static std::atomic<uint32_t> s_dnsBusy{0};  // 拿锁超时累计 → status.dnsbusy

bool httpDnsLock(uint32_t waitMs) {
  if (!s_dnsMtx) return true;  // httpClientBegin 前只有 loop 单任务，无并发
  uint32_t t0 = millis();
  for (;;) {
    if (xSemaphoreTake(s_dnsMtx, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    // loop 侧此刻看门狗还没撤：边等边喂，别把 5s 等成 TWT 复位
    esp_task_wdt_reset();
    if (millis() - t0 >= waitMs) {
      s_dnsBusy++;
      return false;
    }
  }
}

void httpDnsUnlock() {
  if (s_dnsMtx) xSemaphoreGive(s_dnsMtx);
}

uint32_t httpDnsBusyCount() { return s_dnsBusy.load(); }

static int httpExchange(const HttpJob& j, String* respOut) {
  IPAddress addr;
  bool dnsOk = false;
  crashSnapMark("http.dns");
  // worker 不受 loop 看门狗约束，但也不能无限等：loop 侧查一次最坏 ~16s
  if (httpDnsLock(HTTP_DNS_LOCK_WAIT_MS)) {
    crashSnapCapture();  // 锁内是当初 dda0 挂死的现场，进去前留一份栈
    dnsOk = WiFi.hostByName(j.host.c_str(), addr);
    httpDnsUnlock();
  }
  if (!dnsOk) return -11;
  WiFiClient client;
  crashSnapMark("http.conn");
  if (!client.connect(addr, j.port, (int32_t)j.timeoutMs)) return -1;
  crashSnapMark("http.io");

  // 静态头缓冲 + body 分段 write：避免碎片堆上 String 拼接 abort
  int n;
  if (j.isPost) {
    n = snprintf(s_reqHdr, sizeof(s_reqHdr),
                 "POST %s HTTP/1.1\r\nHost: %s\r\n"
                 "User-Agent: garage-esp32\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n\r\n",
                 j.path.c_str(), j.host.c_str(),
                 (unsigned)j.body.length());
  } else {
    n = snprintf(s_reqHdr, sizeof(s_reqHdr),
                 "GET %s HTTP/1.1\r\nHost: %s\r\n"
                 "User-Agent: garage-esp32\r\n"
                 "Accept: application/json\r\n"
                 "Connection: close\r\n\r\n",
                 j.path.c_str(), j.host.c_str());
  }
  if (n <= 0 || n >= (int)sizeof(s_reqHdr)) {
    client.stop();
    return -2;
  }
  if (client.print(s_reqHdr) != n) {
    client.stop();
    return -2;
  }
  if (j.isPost && j.body.length() > 0) {
    const char* bp = j.body.c_str();
    size_t blen = j.body.length();
    size_t off = 0;
    while (off < blen) {
      size_t chunk = blen - off;
      if (chunk > 256) chunk = 256;
      size_t w = client.write((const uint8_t*)(bp + off), chunk);
      if (w == 0) {
        client.stop();
        return -2;
      }
      off += w;
    }
  }

  uint32_t start = millis();
  size_t rawN = 0;
  while (client.connected() || client.available()) {
    if (millis() - start > j.timeoutMs) {
      client.stop();
      return -3;
    }
    while (client.available() && rawN + 1 < sizeof(s_rawBuf)) {
      int c = client.read();
      if (c < 0) break;
      s_rawBuf[rawN++] = (char)c;
    }
    s_rawBuf[rawN] = 0;
    if (rawN >= 4 && strstr(s_rawBuf, "\r\n\r\n") && !client.connected()) break;
    vTaskDelay(pdMS_TO_TICKS(1));
    if (rawN >= sizeof(s_rawBuf) - 1) break;
  }
  uint32_t tail = millis();
  while (client.available() && rawN + 1 < sizeof(s_rawBuf) &&
         millis() - tail < 500) {
    int c = client.read();
    if (c < 0) break;
    s_rawBuf[rawN++] = (char)c;
  }
  s_rawBuf[rawN] = 0;
  client.stop();

  char* hdrEnd = strstr(s_rawBuf, "\r\n\r\n");
  if (!hdrEnd) return -4;
  size_t bodyOff = (size_t)(hdrEnd - s_rawBuf) + 4;
  if (respOut) {
    // 响应体仍拷一次 String（消费方 API）；失败则空串，不 abort
    respOut->remove(0);
    if (rawN > bodyOff) {
      respOut->concat(s_rawBuf + bodyOff, rawN - bodyOff);
    }
  }
  // 状态行: HTTP/1.1 200 OK
  int sp1 = -1, sp2 = -1;
  for (size_t i = 0; i < bodyOff && i < 32; i++) {
    if (s_rawBuf[i] == ' ') {
      if (sp1 < 0) sp1 = (int)i;
      else {
        sp2 = (int)i;
        break;
      }
    }
  }
  if (sp1 < 0 || sp2 < 0 || sp2 <= sp1 + 1) return -5;
  return atoi(s_rawBuf + sp1 + 1);
}

// 单次任务完成：代次一致才交付结果（被看门狗判死的旧任务直接丢弃）
static void httpFinishJob(HttpJob* j, int code, const String& resp) {
  HttpSlot& s = s_slots[j->owner];
  if (s.gen == j->gen) {
    s.code = code;
    s.body = resp;
    s.ready.store(true);  // 数据先写，ready 后置
  }
  if (code < 0 && code != -13) {
    s_netFail++;  // DNS/connect/超时等网络层失败；-13=inquiry 保护放弃，不计入
  } else if (code >= 0) {
    s_netFail = 0;
  }
  delete j;
  s_workerBusy.store(false);
}

static void httpWorker(void*) {
  HttpJob* j = nullptr;
  for (;;) {
    if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE || !j) continue;
    s_workerBusy.store(true);
    s_slots[j->owner].startMs = millis();
    crashSnapMark("worker.job");
    crashSnapCapture();  // 每单留一份栈：卡在半路时至少知道从哪出发

    if (WiFi.status() != WL_CONNECTED) {
      httpFinishJob(j, -10, String());  // ownerBusy 由消费方收结果时清
      continue;
    }

    // NFC probe/hwInit 期间禁止出向：避免 nfc.probe + http.io 同时碰碎片堆
    if (s_nfcBusy.load()) {
      httpFinishJob(j, -13, String());
      continue;
    }

    // Inquiry 优先：只在空窗发送；短等后仍 btBusy → 放弃本单（-13），
    // 不推迟 inquiry，也不把放弃算进 netfail（否则会误触发 force STA reconnect）
    s_radioBusy.store(true);
    uint32_t t0 = millis();
    while (s_webBusy.load() || (s_btBusy && s_btBusy()) || s_nfcBusy.load()) {
      if (millis() - t0 > HTTP_BT_GAP_WAIT_MS) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_btBusy && s_btBusy()) {
      httpFinishJob(j, -13, String());
      s_radioBusy.store(false);
      continue;
    }
    if (s_nfcBusy.load()) {
      httpFinishJob(j, -13, String());
      s_radioBusy.store(false);
      continue;
    }

    String resp;
    int code = httpExchange(*j, &resp);
    s_radioBusy.store(false);
    httpFinishJob(j, code, resp);
  }
}

static TaskHandle_t s_workerTask = nullptr;

void httpClientBegin(HttpBtBusyFn btBusyFn) {
  s_btBusy = btBusyFn;
  // DNS 互斥必须在 worker 起来之前就绪，否则首包可能绕过串行化
  if (!s_dnsMtx) s_dnsMtx = xSemaphoreCreateMutex();
  for (int i = 0; i < HTTP_OWNER_COUNT; i++) {
    s_ownerBusy[i].store(false);
    s_slots[i].ready.store(false);
  }
  s_jobs = xQueueCreate(6, sizeof(HttpJob*));
  xTaskCreatePinnedToCore(httpWorker, "httpWorker", HTTP_TASK_STACK, nullptr,
                          HTTP_TASK_PRIO, &s_workerTask, 0);
}

uint32_t httpClientWorkerStackHwm() {
  if (!s_workerTask) return 0;
  return (uint32_t)uxTaskGetStackHighWaterMark(s_workerTask) *
         sizeof(StackType_t);
}

static bool submit(int owner, bool isPost, const String& host, uint16_t port,
                   const String& path, const String& body,
                   uint32_t timeoutMs) {
  if (!s_jobs || owner < 0 || owner >= HTTP_OWNER_COUNT) return false;
  if (s_paused.load()) return false;  // OTA 排空期拒新单
  // NFC probe/hwInit：拒新单，避免与 nfc.probe 并发碰碎片堆
  if (s_nfcBusy.load()) return false;
  // Inquiry/BLE 占用时不入队：只在 inquiry 空窗发 HTTP，避免挤掉 BTU 4112 连续块
  if (s_btBusy && s_btBusy()) return false;
  if (s_ownerBusy[owner].load()) return false;  // 该 owner 已有在飞请求
  s_ownerBusy[owner].store(true);
  HttpSlot& s = s_slots[owner];
  s.gen++;
  s.submitMs = millis();
  s.startMs = 0;
  s.timeoutMs = timeoutMs;
  // 必须 nothrow：本工具链异常关闭，普通 new 在 OOM 时走 std::terminate→abort→
  // rst=4 panic，下面的 !j 保护永远走不到。dda0 实测两次 panic 的崩溃指纹都是
  // loop 停在 ls.flush（日志提交 http）+ 堆碎片峰值（2308/4112 连续失败），
  // 且 panic 重启不经过 OTA 静默 → PN532 卡死 → 界面误报"无芯片"。
  // nothrow 失败返回空指针，走下面既有的回填/失败路径。
  HttpJob* j = new (std::nothrow)
      HttpJob{owner, s.gen, isPost, host, port, path, body, timeoutMs};
  // String 拷贝也可能在碎片堆里失败（静默空串），一并按提交失败处理
  if (!j || j->host.length() != host.length() ||
      j->path.length() != path.length() ||
      j->body.length() != body.length()) {
    s.submitMs = 0;
    s_ownerBusy[owner].store(false);
    delete j;
    return false;
  }
  if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
    s.submitMs = 0;
    s_ownerBusy[owner].store(false);
    delete j;
    return false;
  }
  return true;
}

bool httpSubmitGet(int owner, const String& host, uint16_t port,
                   const String& path, uint32_t timeoutMs) {
  return submit(owner, false, host, port, path, String(), timeoutMs);
}

bool httpSubmitPost(int owner, const String& host, uint16_t port,
                    const String& path, const String& body,
                    uint32_t timeoutMs) {
  return submit(owner, true, host, port, path, body, timeoutMs);
}

bool httpTryResult(int owner, int* code, String* body) {
  if (owner < 0 || owner >= HTTP_OWNER_COUNT) return false;
  HttpSlot& s = s_slots[owner];
  if (!s.ready.load() && s_ownerBusy[owner].load()) {
    // 在飞看门狗：结果丢失（null 任务被丢弃/worker 卡死）时合成失败释放，
    // 否则该 owner 的 inFlight 永远等不到结果 → 这类请求死到重启为止
    uint32_t now = millis();
    bool stuck;
    if (s.startMs == 0) {
      stuck = s.submitMs != 0 && (now - s.submitMs) > HTTP_STUCK_UNPICKED_MS;
    } else {
      stuck = (now - s.startMs) > s.timeoutMs + HTTP_STUCK_EXTRA_MS;
    }
    if (stuck) {
      s.gen++;  // 迟到的真实结果按代次丢弃，不污染下一个请求
      s.code = -12;
      s.body = "";
      s.ready.store(true);
      Serial.printf("[HTTP] owner=%d 在飞超时 (picked=%d) -> 合成 -12 释放\n",
                    owner, (int)(s.startMs != 0));
    }
  }
  if (!s.ready.load()) return false;
  if (code) *code = s.code;
  if (body) *body = s.body;
  s.body = "";
  s.ready.store(false);
  s.submitMs = 0;
  s.startMs = 0;
  s_ownerBusy[owner].store(false);  // 消费完才允许该 owner 再提交
  return true;
}

bool httpClientBusy() { return s_radioBusy.load(); }

bool httpClientWebBusy() { return s_webBusy.load(); }

void httpSetWebBusy(bool busy) {
  bool was = s_webBusy.load();
  s_webBusy.store(busy);
  // 网页响应结束：发送期间出向请求可能被挤到失败，清掉 netfail 连击，
  // 否则数据面看门狗会把刚发完网页的 WiFi 拆掉（实测一拆掉线 78s）
  if (was && !busy) httpClientResetNetFail();
}

bool httpPause(uint32_t waitMs) {
  s_paused.store(true);
  uint32_t t0 = millis();
  while (uxQueueMessagesWaiting(s_jobs) > 0 || s_workerBusy.load()) {
    if (millis() - t0 > waitMs) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_task_wdt_reset();  // 在 loop 上下文最长等 4s，喂狗防 TWT
  }
  return true;
}

void httpResume() { s_paused.store(false); }

int httpClientNetFailStreak() { return s_netFail.load(); }
void httpClientResetNetFail() { s_netFail.store(0); }
