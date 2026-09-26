#include "http_client.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// 单工作任务：所有 HTTP 在这里串行执行，loop 永不阻塞。
// 射频仲裁：发送前等蓝牙空隙（有上限），发送中置 radioBusy 给 inquiry 让路。

#ifndef HTTP_TASK_STACK
#define HTTP_TASK_STACK 8192
#endif
#ifndef HTTP_TASK_PRIO
#define HTTP_TASK_PRIO 1
#endif
// 等蓝牙让路的上限：超时就直接发（防饿死）
#ifndef HTTP_BT_WAIT_MAX_MS
#define HTTP_BT_WAIT_MAX_MS 4000
#endif

struct HttpJob {
  int owner;
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
};

static QueueHandle_t s_jobs = nullptr;
static HttpSlot s_slots[HTTP_OWNER_COUNT];
static std::atomic<bool> s_ownerBusy[HTTP_OWNER_COUNT];
static std::atomic<bool> s_radioBusy{false};
static HttpBtBusyFn s_btBusy = nullptr;

static int httpExchange(const HttpJob& j, String* respOut) {
  IPAddress addr;
  if (!WiFi.hostByName(j.host.c_str(), addr)) return -11;
  WiFiClient client;
  if (!client.connect(addr, j.port, (int32_t)j.timeoutMs)) return -1;

  String req;
  req.reserve(160 + j.body.length());
  if (j.isPost) {
    req += "POST ";
    req += j.path;
    req += " HTTP/1.1\r\nHost: ";
    req += j.host;
    req += "\r\nUser-Agent: garage-esp32\r\n";
    req += "Content-Type: text/plain\r\nContent-Length: ";
    req += String((unsigned)j.body.length());
    req += "\r\nConnection: close\r\n\r\n";
    req += j.body;
  } else {
    req += "GET ";
    req += j.path;
    req += " HTTP/1.1\r\nHost: ";
    req += j.host;
    req += "\r\nUser-Agent: garage-esp32\r\nAccept: application/json\r\n";
    req += "Connection: close\r\n\r\n";
  }
  if (client.print(req) != (int)req.length()) {
    client.stop();
    return -2;
  }

  uint32_t start = millis();
  String raw;
  raw.reserve(512);
  while (client.connected() || client.available()) {
    if (millis() - start > j.timeoutMs) {
      client.stop();
      return -3;
    }
    while (client.available()) raw += (char)client.read();
    if (raw.indexOf("\r\n\r\n") >= 0 && !client.connected()) break;
    vTaskDelay(pdMS_TO_TICKS(1));
    if (raw.length() > 8192) break;
  }
  uint32_t tail = millis();
  while (client.available() && millis() - tail < 500) {
    raw += (char)client.read();
  }
  client.stop();

  int hdrEnd = raw.indexOf("\r\n\r\n");
  if (hdrEnd < 0) return -4;
  String head = raw.substring(0, hdrEnd);
  if (respOut) *respOut = raw.substring(hdrEnd + 4);
  int sp1 = head.indexOf(' ');
  int sp2 = head.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return -5;
  return head.substring(sp1 + 1, sp2).toInt();
}

static void httpWorker(void*) {
  HttpJob* j = nullptr;
  for (;;) {
    if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE || !j) continue;

    if (WiFi.status() != WL_CONNECTED) {
      s_slots[j->owner].code = -10;
      s_slots[j->owner].body = "";
      s_slots[j->owner].ready.store(true);  // ownerBusy 由消费方收结果时清
      delete j;
      continue;
    }

    // 先占射频标志（BleTracker 见状推迟新 inquiry），再等正在跑的让路
    s_radioBusy.store(true);
    uint32_t t0 = millis();
    while (s_btBusy && s_btBusy()) {
      if (millis() - t0 > HTTP_BT_WAIT_MAX_MS) break;
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    String resp;
    int code = httpExchange(*j, &resp);
    s_radioBusy.store(false);

    s_slots[j->owner].code = code;
    s_slots[j->owner].body = resp;
    s_slots[j->owner].ready.store(true);  // 数据先写，ready 后置
    delete j;
    j = nullptr;
  }
}

void httpClientBegin(HttpBtBusyFn btBusyFn) {
  s_btBusy = btBusyFn;
  for (int i = 0; i < HTTP_OWNER_COUNT; i++) {
    s_ownerBusy[i].store(false);
    s_slots[i].ready.store(false);
  }
  s_jobs = xQueueCreate(6, sizeof(HttpJob*));
  xTaskCreatePinnedToCore(httpWorker, "httpWorker", HTTP_TASK_STACK, nullptr,
                          HTTP_TASK_PRIO, nullptr, 0);
}

static bool submit(int owner, bool isPost, const String& host, uint16_t port,
                   const String& path, const String& body,
                   uint32_t timeoutMs) {
  if (!s_jobs || owner < 0 || owner >= HTTP_OWNER_COUNT) return false;
  if (s_ownerBusy[owner].load()) return false;  // 该 owner 已有在飞请求
  s_ownerBusy[owner].store(true);
  HttpJob* j = new HttpJob{owner, isPost, host, port, path, body, timeoutMs};
  if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
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
  if (!s.ready.load()) return false;
  if (code) *code = s.code;
  if (body) *body = s.body;
  s.body = "";
  s.ready.store(false);
  s_ownerBusy[owner].store(false);  // 消费完才允许该 owner 再提交
  return true;
}

bool httpClientBusy() { return s_radioBusy.load(); }
