#pragma once
#include <Arduino.h>
#include <ESP.h>
#include "config.h"

// 多设备身份：与 SoftAP/mDNS 后缀一致（芯片 MAC 低 16 位）
inline String deviceId() {
  static String id;
  if (!id.length()) {
    uint64_t chipid = ESP.getEfuseMac();
    char buf[8];
    snprintf(buf, sizeof(buf), "%04x", (unsigned)(chipid & 0xFFFF));
    id = String("garage-") + buf;
#ifdef DEVICE_ROLE
    // 测试板后缀，避免和主门列表混淆
    if (String(DEVICE_ROLE) == "lab") id += "-lab";
#endif
  }
  return id;
}
