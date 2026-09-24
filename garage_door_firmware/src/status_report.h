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
  bool webUp = true;
  bool sta = false;
  bool ap = false;
  bool rfOpen = false;
  bool rfClose = false;
  bool rfTxBusy = false;
  bool remoteOn = false;
  int door = 0;
  int rssi = 0;
  uint32_t heap = 0;
  uint32_t maxblk = 0;
  uint32_t uptimeMs = 0;
  const char* role = "door";
};

void statusReportSetBits(const StatusBits& b);
