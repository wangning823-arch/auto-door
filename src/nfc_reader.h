#pragma once
#include <Arduino.h>

// PN532 NFC（I2C）：读卡 UID，匹配后触发开关门
class NfcReader {
 public:
  bool begin(int sda, int scl);
  bool ok() const { return ok_; }

  // 非阻塞轮询：读到卡返回 true（uid 为十六进制大写字符串）
  bool poll(String& uid);

  void setAuthUid(const String& uid) { authUid_ = uid; }
  String authUid() const { return authUid_; }
  bool isAuthorized(const String& uid) const;

  String debugLine() const;

 private:
  bool ok_ = false;
  String authUid_;   // 授权卡
  String lastUid_;   // 最近读到（防重复触发）
  uint32_t lastReadMs_ = 0;
};
