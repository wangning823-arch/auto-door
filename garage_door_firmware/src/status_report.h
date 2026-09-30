#pragma once
#include <Arduino.h>
#include "config.h"

// 周期把运行状态 POST 到 VPS /dev/status（NFC/Web/RF/版本…）
void statusReportBegin();
// btBusy 时不抢射频；wifiOk=STA 已连
void statusReportService(bool btBusy, bool wifiOk);
void statusReportNow();

struct StatusBits {
  bool nfcOk = false;
  bool nfcDeferred = false;
  bool nfcAbsent = false;
  bool nfcListen = true;
  bool webUp = true;   // 本地网页开关状态（VPS 控制台接管后=web_ui）
  bool sta = false;
  bool ap = false;
  bool rfOpen = false;
  bool rfClose = false;
  bool rfTxBusy = false;
  bool remoteOn = false;
  int door = 0;
  int rssi = 0;        // WiFi RSSI
  uint32_t heap = 0;
  uint32_t maxblk = 0;
  uint32_t uptimeMs = 0;
  const char* role = "door";
  // ===== VPS 控制台展示/配置回显（替代本地网页）=====
  char staIp[20] = "";
  char mac[24] = "";
  char bleLab[32] = "";
  int trackMode = 0;     // 0=BLE 1=经典
  bool autoTrack = false;
  bool pairOpen = false;
  bool pairHasPin = false;
  int bleRssi = -127;    // 已配对手机（BLE 模式）
  int carRssi = -127;    // 车机（经典模式）
  int trend = 0;         // SignalTrend
  // 精确堆统计（开机累计，非 HEAPFAIL 抽样）
  uint32_t heapFailN = 0;   // 所有 malloc 失败
  uint32_t btuFailN = 0;    // BTU 4112 / BTU_TASK 失败
  uint32_t thinN = 0;       // inquiry 前 maxblk<4112 次数
  uint32_t inqN = 0;        // inquiry 启动次数
};

void statusReportSetBits(const StatusBits& b);
