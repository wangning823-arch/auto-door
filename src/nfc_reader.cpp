#include "nfc_reader.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_PN532.h>

// I2C：SDA/SCL
static Adafruit_PN532 nfc(PIN_NFC_SDA, PIN_NFC_SCL);

#define NFC_COOLDOWN_MS 1500  // 同一张卡 1.5s 内不重复触发

bool NfcReader::begin(int sda, int scl) {
  Wire.begin(sda, scl);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC] PN532 未找到，检查接线/焊盘是否拨到 I2C");
    ok_ = false;
    return false;
  }
  Serial.printf("[NFC] PN532 固件: 0x%08X\n", ver);
  nfc.SAMConfig();  // 常开模式
  ok_ = true;
  return true;
}

bool NfcReader::poll(String& uid) {
  if (!ok_) return false;

  uint8_t buf[16];
  uint8_t len = 0;
  // 读 MIFARE Classic / NTAG 等，超时很短（内部非阻塞）
  uint8_t ret = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, buf, &len, 100);
  if (!ret || len < 4) return false;

  uid = "";
  for (uint8_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) uid += "0";
    uid += String(buf[i], HEX);
  }
  uid.toUpperCase();

  uint32_t now = millis();
  if (uid == lastUid_ && (now - lastReadMs_) < NFC_COOLDOWN_MS) {
    return false;  // 同卡冷却中
  }
  lastUid_ = uid;
  lastReadMs_ = now;
  return true;
}

bool NfcReader::isAuthorized(const String& uid) const {
  if (authUid_.length() == 0) return false;  // 未注册任何卡
  return uid.equalsIgnoreCase(authUid_);
}

String NfcReader::debugLine() const {
  return "nfc=" + String(ok_ ? "ok" : "fail") +
         " auth=" + (authUid_.length() ? authUid_ : String("-"));
}
