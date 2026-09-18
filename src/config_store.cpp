#include "config_store.h"
#include <Preferences.h>

static Preferences prefs;

void ConfigStore::begin() {
  prefs.begin("gdoor", false);  // RW
  ready_ = true;
  Serial.println("[CFG] NVS ready");
}

String ConfigStore::loadMac(const char* defaultMac) {
  if (!ready_) return String(defaultMac);
  String m = prefs.getString("car_mac", defaultMac);
  m.trim();
  if (m.length() < 11) return String(defaultMac);
  return m;
}

bool ConfigStore::saveMac(const String& mac) {
  if (!ready_) return false;
  return prefs.putString("car_mac", mac) > 0;
}

bool ConfigStore::clearMac() {
  if (!ready_) return false;
  return prefs.remove("car_mac");
}

bool ConfigStore::loadWifiEnabled(bool defaultOn) {
  if (!ready_) return defaultOn;
  return prefs.getBool("wifi_on", defaultOn);
}

bool ConfigStore::saveWifiEnabled(bool on) {
  if (!ready_) return false;
  return prefs.putBool("wifi_on", on);
}

String ConfigStore::loadBleFilter() {
  if (!ready_) return String();
  return prefs.getString("ble_filter", "");
}

bool ConfigStore::saveBleFilter(const String& f) {
  if (!ready_) return false;
  if (f.length() == 0) return prefs.remove("ble_filter") || true;
  return prefs.putString("ble_filter", f) > 0;
}

int ConfigStore::loadTrackMode(int defaultMode) {
  if (!ready_) return defaultMode;
  return prefs.getInt("track_mode", defaultMode);
}

bool ConfigStore::saveTrackMode(int mode) {
  if (!ready_) return false;
  return prefs.putInt("track_mode", mode);
}

String ConfigStore::loadNfcUid() {
  if (!ready_) return String();
  return prefs.getString("nfc_uid", "");
}

bool ConfigStore::saveNfcUid(const String& uid) {
  if (!ready_) return false;
  return prefs.putString("nfc_uid", uid) > 0;
}

bool ConfigStore::clearNfcUid() {
  if (!ready_) return false;
  return prefs.remove("nfc_uid");
}
