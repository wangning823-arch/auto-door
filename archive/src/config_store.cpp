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

String ConfigStore::loadStaSsid() {
  if (!ready_) return String();
  String s = prefs.getString("sta_ssid", "");
  s.trim();
  return s;
}

String ConfigStore::loadStaPass() {
  if (!ready_) return String();
  return prefs.getString("sta_pass", "");
}

bool ConfigStore::saveSta(const String& ssid, const String& pass) {
  if (!ready_) return false;
  String s = ssid;
  s.trim();
  if (s.length() == 0) return clearSta();
  if (prefs.putString("sta_ssid", s) == 0) return false;
  prefs.putString("sta_pass", pass);
  return true;
}

bool ConfigStore::clearSta() {
  if (!ready_) return false;
  prefs.remove("sta_pass");
  return prefs.remove("sta_ssid") || true;
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

String ConfigStore::loadRfKey(int idx) {
  if (!ready_ || idx < 0 || idx > 3) return String();
  char k[8];
  snprintf(k, sizeof(k), "rfk%d", idx);
  return prefs.getString(k, "");
}

bool ConfigStore::saveRfKey(int idx, const char* csv) {
  if (!ready_ || idx < 0 || idx > 3 || !csv) return false;
  char k[8];
  snprintf(k, sizeof(k), "rfk%d", idx);
  return prefs.putString(k, csv) > 0;
}

bool ConfigStore::clearRfKey(int idx) {
  if (!ready_ || idx < 0 || idx > 3) return false;
  char k[8];
  snprintf(k, sizeof(k), "rfk%d", idx);
  return prefs.remove(k);
}

bool ConfigStore::loadRfAuto(bool defaultOn) {
  if (!ready_) return defaultOn;
  return prefs.getBool("rf_auto", defaultOn);
}

bool ConfigStore::saveRfAuto(bool on) {
  if (!ready_) return false;
  return prefs.putBool("rf_auto", on);
}

bool ConfigStore::loadRemote(bool defaultOn) {
  if (!ready_) return defaultOn;
  return prefs.getBool("remote_on", defaultOn);
}

bool ConfigStore::saveRemote(bool on) {
  if (!ready_) return false;
  return prefs.putBool("remote_on", on);
}

bool ConfigStore::loadAutoTrack(bool defaultOn) {
  if (!ready_) return defaultOn;
  return prefs.getBool("auto_trk", defaultOn);
}

bool ConfigStore::saveAutoTrack(bool on) {
  if (!ready_) return false;
  return prefs.putBool("auto_trk", on);
}
