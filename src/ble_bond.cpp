#include "ble_bond.h"
#include "ble_scan.h"

// gBleScan 在 main.cpp 定义（非 static）
extern BleScanTool gBleScan;

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BLUEDROID_ENABLED)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>
#include <esp_gatts_api.h>
#include <mbedtls/aes.h>
#include <string.h>

static Preferences bondPrefs;
static BLEServer* gServer = nullptr;
static BLESecurity* gSecurity = nullptr;
static bool gAdvOn = false;

BleBond gBleBond;

static String macFromNative(const uint8_t* a) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2],
           a[3], a[4], a[5]);
  return String(buf);
}

static bool parseMac(const String& s, uint8_t out[6]) {
  if (s.length() != 17) return false;
  int d[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &d[0], &d[1], &d[2], &d[3], &d[4],
             &d[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)d[i];
  return true;
}

static bool resolveRpa(const uint8_t addr[6], const uint8_t irk[16]) {
  if ((addr[0] & 0xC0) != 0x40) return false;  // prand 两位标志 01
  // 对齐 Bluedroid btm_ble_resolve_rpa + SMP_Encrypt：
  // plain = (a2,a1,a0)||0^104 后整体 reverse；key 也 reverse；AES 后输出再 reverse
  // 与 hash=(a5,a4,a3) 比前 3 字节
  uint8_t plain[16] = {0};
  plain[0] = addr[2];
  plain[1] = addr[1];
  plain[2] = addr[0];
  uint8_t plainRev[16];
  uint8_t keyRev[16];
  for (int i = 0; i < 16; i++) {
    plainRev[i] = plain[15 - i];
    keyRev[i] = irk[15 - i];
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, keyRev, 128);
  uint8_t out[16];
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plainRev, out);
  mbedtls_aes_free(&aes);
  uint8_t x0 = out[15], x1 = out[14], x2 = out[13];
  return x0 == addr[5] && x1 == addr[4] && x2 == addr[3];
}

class SecCbs : public BLESecurityCallbacks {
 public:
  void onPassKeyNotify(uint32_t key) override {
    Serial.printf("[BOND] ===== 手机请输入 PIN: %06u =====\n", (unsigned)key);
  }
  uint32_t onPassKeyRequest() override {
    uint32_t k = gBleBond.staticPasskey();
    Serial.printf("[BOND] onPassKeyRequest -> %06u\n", (unsigned)k);
    return k;
  }
  bool onSecurityRequest() override {
    bool ok = gBleBond.allowSmp();
    Serial.printf("[BOND] SMP security_req accept=%d open=%d\n", (int)ok,
                  (int)gBleBond.pairingOpen());
    return ok;
  }
  bool onConfirmPIN(uint32_t pin) override {
    bool ok = gBleBond.allowSmp();
    if (gBleBond.hasPasskey()) {
      ok = ok && (pin == gBleBond.staticPasskey());
      Serial.printf("[BOND] PIN confirm %06u ok=%d\n", (unsigned)pin,
                    (int)ok);
    }
    return ok;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    if (cmpl.success) {
      Serial.printf("[BOND] 配对成功 %s auth_mode=0x%02x\n",
                    macFromNative(cmpl.bd_addr).c_str(),
                    (unsigned)cmpl.auth_mode);
      // 禁止在 GAP 回调里读 bond/NVS（易拖垮 SoftAP）；只记下 peer
      gBleBond.notePeer(cmpl.bd_addr);
      gBleBond.requestDelayedClose("配对完成", 3000);
    } else {
      Serial.printf("[BOND] 配对失败 reason=%u\n", cmpl.fail_reason);
    }
  }
};

class SrvCbs : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {}
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
    if (!gBleBond.pairingOpen()) {
      Serial.println("[BOND] 关窗收到连接 → 立刻断开");
      if (p && gServer) {
        gServer->disconnect(p->connect.conn_id);
      }
      return;
    }
    if (p) esp_ble_set_encryption(p->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
    Serial.println("[BOND] 开窗：请求 MITM 加密");
  }
  void onDisconnect(BLEServer* s) override {
    Serial.println("[BOND] BLE 断开");
    if (s && gBleBond.pairingOpen()) {
      s->startAdvertising();
      gAdvOn = true;
    } else if (s) {
      s->getAdvertising()->setAdvertisementType(ADV_TYPE_NONCONN_IND);
      BLEDevice::stopAdvertising();
      gAdvOn = false;
    }
  }
};

static void onGap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (event == ESP_GAP_BLE_KEY_EVT && param) {
    auto& k = param->ble_security.ble_key;
    Serial.printf("[BOND] KEY_EVT type=0x%02x from %s open=%d\n", k.key_type,
                  macFromNative(k.bd_addr).c_str(), (int)gBleBond.pairingOpen());
    // 配对窗口内或延迟关窗期间都接受 PID；AUTH_CMPL 后 2.5s 内仍 open
    if (k.key_type & ESP_LE_KEY_PID) {
      const auto& pid = k.p_key_value.pid_key;
      uint8_t z[6] = {0};
      uint8_t id[6];
      if (memcmp(pid.static_addr, z, 6) != 0)
        memcpy(id, pid.static_addr, 6);
      else
        memcpy(id, k.bd_addr, 6);
      if (gBleBond.pairingOpen()) {
        gBleBond.savePeerIdKey(pid.irk, id);
      } else {
        Serial.println("[BOND] PID 到达但窗口已关");
        gBleBond.notePeer(k.bd_addr);
      }
    }
  } else if (event == ESP_GAP_BLE_AUTH_CMPL_EVT && param) {
    auto& c = param->ble_security.auth_cmpl;
    Serial.printf("[BOND] AUTH_CMPL success=%d id=%s\n", (int)c.success,
                  macFromNative(c.bd_addr).c_str());
    if (c.success) gBleBond.notePeer(c.bd_addr);
  } else if (event == ESP_GAP_BLE_SEC_REQ_EVT && param) {
    bool accept = gBleBond.allowSmp();
    if (!accept) {
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, false);
      Serial.println("[BOND] SEC_REQ REJECT（配对已关）");
    } else {
      Serial.println("[BOND] SEC_REQ accept（窗口开）");
    }
    return;
  }
}

bool BleBond::pairingOpen() const {
  if (!pairWin_) return false;
  if (pairWinSticky_) return true;
  return (int32_t)(pairWinEndMs_ - millis()) > 0;
}

bool BleBond::hasPasskey() const { return pin_.length() == 6; }

uint32_t BleBond::staticPasskey() const {
  if (!hasPasskey()) return 0;
  return (uint32_t)pin_.toInt();
}

void BleBond::setPairingPin(const String& pin) {
  String p = pin;
  p.trim();
  if (p.length() && p.length() != 6) {
    Serial.println("[BOND] PIN 必须是 6 位数字，已忽略");
    return;
  }
  if (p.length()) {
    for (unsigned i = 0; i < p.length(); i++) {
      if (p[i] < '0' || p[i] > '9') {
        Serial.println("[BOND] PIN 只能是数字，已忽略");
        return;
      }
    }
  }
  pin_ = p;
  bondPrefs.begin("blebond", false);
  if (pin_.length())
    bondPrefs.putString("pin", pin_);
  else
    bondPrefs.remove("pin");
  bondPrefs.end();
  applyBleSecurity();
  Serial.printf("[BOND] 手机配对 PIN %s\n",
                pin_.length() ? "已设置为6位（手机配对时输入）" : "已清除（Just Works）");
}

void BleBond::applyBleSecurity() {
  if (!gSecurity) return;
  // 只写 GAP 参数，不调 setStaticPIN（它会把 auth 改成 SC_ONLY、丢掉 MITM/BOND）
  esp_ble_auth_req_t auth;
  esp_ble_io_cap_t iocap;
  uint32_t pass = hasPasskey() ? staticPasskey() : 0;
  uint8_t keySz = 16;
  uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  if (hasPasskey()) {
    auth = (esp_ble_auth_req_t)(ESP_LE_AUTH_REQ_MITM | ESP_LE_AUTH_BOND);
    iocap = ESP_IO_CAP_OUT;  // ESP 出示 6 位，手机键盘输入
  } else {
    auth = ESP_LE_AUTH_REQ_SC_BOND;
    iocap = ESP_IO_CAP_NONE;
  }
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth,
                                  sizeof(auth));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
  if (hasPasskey()) {
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pass,
                                   sizeof(pass));
  }
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySz, sizeof(keySz));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));

  // Arduino 包装：只设 IO/密钥，auth 用上面的 raw 值再写一次防被覆盖
  gSecurity->setCapability(iocap);
  gSecurity->setKeySize(16);
  gSecurity->setInitEncryptionKey(keys);
  gSecurity->setRespEncryptionKey(keys);
  gSecurity->setAuthenticationMode(auth);

  Serial.printf("[BOND] auth=0x%02x iocap=%d PIN=%s\n", (unsigned)auth,
                (int)iocap,
                hasPasskey() ? String(staticPasskey()).c_str() : "none");
}

void BleBond::setAdvConnectable(bool connectable) {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if (!adv) return;
  // 关窗：不可连接 → 手机能扫到也无法配对；开窗：可连接广播
  adv->setAdvertisementType(connectable ? ADV_TYPE_IND : ADV_TYPE_NONCONN_IND);
  adv->setScanResponse(true);
}

void BleBond::startAdv() {
  if (!gServer) return;
  setAdvConnectable(true);
  BLEDevice::getAdvertising()->start();
  gAdvOn = true;
  Serial.println("[BOND] adv START connectable=1");
}

void BleBond::stopAdv() {
  // 关窗：先改成不可连接再停；若停失败也不可配对
  setAdvConnectable(false);
  if (gServer) BLEDevice::getAdvertising()->stop();
  BLEDevice::stopAdvertising();
  gAdvOn = false;
  Serial.println("[BOND] adv STOP connectable=0");
}

void BleBond::disconnectAll() {
  if (!gServer) return;
  auto peers = gServer->getPeerDevices(false);
  for (auto& kv : peers) {
    if (kv.second.connected) {
      gServer->disconnect(kv.first);
      Serial.printf("[BOND] 断开 conn=%u\n", kv.first);
    }
  }
}

void BleBond::clearSystemBonds() {
  int count = esp_ble_get_bond_device_num();
  if (count <= 0) return;
  if (count > 8) count = 8;
  esp_ble_bond_dev_t* list =
      (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * count);
  if (!list) return;
  int n = count;
  if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
    for (int i = 0; i < n; i++) {
      esp_ble_remove_bond_device(list[i].bd_addr);
      Serial.printf("[BOND] 移除 bond %s\n",
                    macFromNative(list[i].bd_addr).c_str());
    }
  }
  free(list);
}

void BleBond::requestOpenPairing(uint32_t ms) {
  if (ms == 0) {
    pendingOpen_ = true;
    pendingOpenMs_ = 0;
  } else {
    if (ms < 15000) ms = 15000;
    if (ms > 300000) ms = 300000;
    pendingOpen_ = true;
    pendingOpenMs_ = ms;
  }
  pendingOpenAt_ = millis() + 80;  // 等 HTTP 响应完再动射频
  Serial.println("[BOND] 请求开窗（延迟80ms执行，避免打断SoftAP）");
}

void BleBond::doOpen(uint32_t ms) {
  pairWin_ = true;
  if (ms == 0) {
    pairWinSticky_ = true;
    pairWinEndMs_ = 0;
    Serial.println("[BOND] 配对开（保持）");
  } else {
    pairWinSticky_ = false;
    pairWinEndMs_ = millis() + ms;
    Serial.printf("[BOND] 配对开 %us\n", (unsigned)(ms / 1000));
  }
  applyBleSecurity();
  startAdv();
  Serial.println(hasPasskey()
                     ? "[BOND] 开窗可连接广播；手机输入 PIN 配对"
                     : "[BOND] 开窗可连接广播；Just Works");
}

void BleBond::openPairingWindow(uint32_t ms) {
  requestOpenPairing(ms);
}

void BleBond::notePeer(const uint8_t* addr6) {
  if (!addr6) return;
  memcpy(pendingPeer_, addr6, 6);
  hasPendingPeer_ = true;
}

void BleBond::closePairingWindow(const char* why) {
  pendingOpen_ = false;
  pendingClose_ = false;
  bool was = pairWin_;
  pairWin_ = false;
  pairWinSticky_ = false;
  pairWinEndMs_ = 0;
  stopAdv();
  // 延迟一点点再断连接/经典锁死，避开 SoftAP 恢复窗口
  disconnectAll();
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  Serial.printf("[BOND] 配对关（%s）was=%d\n", why ? why : "-", (int)was);
}

void BleBond::service() {
  if (pendingOpen_ && (int32_t)(pendingOpenAt_ - millis()) <= 0) {
    pendingOpen_ = false;
    doOpen(pendingOpenMs_);
  }

  // 主循环里安全地读 bond（不在 GAP 回调里做）
  if (hasPendingPeer_ && !hasIrk_) {
    trySaveFromSystemBond(pendingPeer_);
    hasPendingPeer_ = false;
  }

  if (pendingClose_ && (int32_t)(pendingCloseAt_ - millis()) <= 0) {
    pendingClose_ = false;
    if (pairingOpen()) {
      if (hasPendingPeer_ && !hasIrk_) {
        trySaveFromSystemBond(pendingPeer_);
        hasPendingPeer_ = false;
      }
      if (!hasIrk_) {
        int n = esp_ble_get_bond_device_num();
        if (n > 0) {
          if (n > 8) n = 8;
          esp_ble_bond_dev_t* list =
              (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
          if (list) {
            int cnt = n;
            if (esp_ble_get_bond_device_list(&cnt, list) == ESP_OK && cnt > 0) {
              trySaveFromSystemBond(list[0].bd_addr);
            }
            free(list);
          }
        }
      }
      closePairingWindow(pendingCloseWhy_.c_str());
    }
  }

  if (pairWin_ && !pairWinSticky_ && (int32_t)(pairWinEndMs_ - millis()) <= 0) {
    closePairingWindow("超时");
  }

  if (!pairingOpen()) {
    static uint32_t lastForceStop = 0;
    if (millis() - lastForceStop >= 2000) {
      lastForceStop = millis();
      setAdvConnectable(false);
      if (gAdvOn) {
        Serial.println("[BOND] 关窗仍在广播 → STOP+NONCONN");
        stopAdv();
      } else {
        BLEDevice::stopAdvertising();
      }
      // 不要每秒调 set_scan_mode——与 SoftAP 抢 BT 栈，配对后 Web 会挂
    }
  }
}

bool BleBond::trySaveFromSystemBond(const uint8_t* peerAddr6) {
  if (!peerAddr6) return false;
  if (hasIrk_) return true;  // 已有
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return false;
  if (n > 8) n = 8;
  esp_ble_bond_dev_t* list =
      (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return false;
  int cnt = n;
  bool ok = false;
  if (esp_ble_get_bond_device_list(&cnt, list) == ESP_OK) {
    for (int i = 0; i < cnt; i++) {
      if (memcmp(list[i].bd_addr, peerAddr6, 6) != 0) continue;
      Serial.printf("[BOND] system bond key_mask=0x%x\n",
                    (unsigned)list[i].bond_key.key_mask);
      if (list[i].bond_key.key_mask & ESP_BLE_ID_KEY_MASK) {
        const auto& pid = list[i].bond_key.pid_key;
        uint8_t z[6] = {0};
        uint8_t id[6];
        if (memcmp(pid.static_addr, z, 6) != 0)
          memcpy(id, pid.static_addr, 6);
        else
          memcpy(id, list[i].bd_addr, 6);
        ok = savePeerIdKey(pid.irk, id);
      } else {
        // 无 IRK：至少记下身份地址，便于 UI 显示「已配对」
        identity_ = macFromNative(list[i].bd_addr);
        bondPrefs.begin("blebond", false);
        bondPrefs.putString("id", identity_);
        bondPrefs.end();
        Serial.printf("[BOND] 无PID键，仅记 identity=%s\n", identity_.c_str());
        ok = true;
      }
      break;
    }
  }
  free(list);
  return ok;
}

void BleBond::requestDelayedClose(const char* why, uint32_t ms) {
  pendingClose_ = true;
  pendingCloseAt_ = millis() + ms;
  pendingCloseWhy_ = why ? why : "delayed";
  Serial.printf("[BOND] 延迟关窗 %ums（%s）等 KEY_EVT\n", (unsigned)ms,
                pendingCloseWhy_.c_str());
}

bool BleBond::savePeerIdKey(const uint8_t* irk16, const uint8_t* identity6) {
  if (!irk16 || !identity6) return false;
  if (!pairingOpen()) {
    Serial.println("[BOND] 拒绝保存 IRK：窗口未开");
    return false;
  }
  memcpy(irk_, irk16, 16);
  identity_ = macFromNative(identity6);
  hasIrk_ = true;
  bondPrefs.begin("blebond", false);
  bondPrefs.putBytes("irk", irk_, 16);
  bondPrefs.putString("id", identity_);
  bondPrefs.end();
  // 配对成功后打开 IRK 周期跟踪（gBleScan 在 main.cpp 定义）
  gBleScan.setTrack(true);
  Serial.printf("[BOND] 已保存授权手机 %s，IRK 跟踪 ON\n", identity_.c_str());
  return true;
}

void BleBond::clearBond(const char* why) {
  hasIrk_ = false;
  identity_ = "";
  memset(irk_, 0, 16);
  bondPrefs.begin("blebond", false);
  bondPrefs.remove("irk");
  bondPrefs.remove("id");
  bondPrefs.end();
  clearSystemBonds();
  Serial.printf("[BOND] 已解绑（%s）\n", why ? why : "-");
}

void BleBond::loadFromStore() {
  bondPrefs.begin("blebond", true);
  size_t n = bondPrefs.getBytes("irk", irk_, 16);
  identity_ = bondPrefs.getString("id", "");
  pin_ = bondPrefs.getString("pin", "");
  bondPrefs.end();
  // 有 identity 无 IRK 也算「已配对」（RPA 解析可能失败，但 UI 要显示）
  hasIrk_ = (n == 16);
  if (!hasIrk_ && identity_.length() == 17) hasIrk_ = true;  // UI 用
  // 注意：matchesAddr 在无真实 IRK 时只比 identity
}

void BleBond::begin() {
  loadFromStore();
  pairWin_ = false;
  pairWinSticky_ = false;
  pairWinEndMs_ = 0;
  pendingOpen_ = false;
  pendingClose_ = false;
  hasPendingPeer_ = false;
  if (hasIrk_) {
    Serial.printf("[BOND] 授权手机 %s（默认不广播）\n", identity_.c_str());
  } else {
    Serial.println("[BOND] 尚未绑定手机");
  }
  if (hasPasskey()) {
    Serial.printf("[BOND] 已设手机 PIN %06u\n", (unsigned)staticPasskey());
  }

  if (!BLEDevice::getInitialized()) {
    BLEDevice::init("GarageDoorBLE");
  }
  BLEDevice::setCustomGapHandler(onGap);

  gSecurity = new BLESecurity();
  applyBleSecurity();
  BLEDevice::setSecurityCallbacks(new SecCbs());

  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new SrvCbs());
  BLEService* svc =
      gServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  BLECharacteristic* ch = svc->createCharacteristic(
      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  ch->addDescriptor(new BLE2902());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  adv->setScanResponse(true);
  adv->setAdvertisementType(ADV_TYPE_NONCONN_IND);
  BLEDevice::stopAdvertising();
  gAdvOn = false;
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  Serial.println("[BOND] init: BLE nonconn stopped; classic NON_DISCOVERABLE");
}

bool BleBond::matchesAddr(const String& addrColon) const {
  if (!hasIrk_) return false;
  uint8_t a[6];
  if (!parseMac(addrColon, a)) return false;
  if (identity_.length() == 17) {
    uint8_t id[6];
    if (parseMac(identity_, id) && memcmp(a, id, 6) == 0) return true;
    // BLE 库地址串可能反字节序，再比一次
    uint8_t idr[6] = {id[5], id[4], id[3], id[2], id[1], id[0]};
    if (parseMac(identity_, id) && memcmp(a, idr, 6) == 0) return true;
  }
  bool irkNonZero = false;
  for (int i = 0; i < 16; i++) {
    if (irk_[i]) {
      irkNonZero = true;
      break;
    }
  }
  if (!irkNonZero) return false;
  if (resolveRpa(a, irk_)) return true;
  // IRK 可能按小端存进 bond，再试反转
  uint8_t irkRev[16];
  for (int i = 0; i < 16; i++) irkRev[i] = irk_[15 - i];
  return resolveRpa(a, irkRev);
}

void BleBond::debugDump() {
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", irk_[i]);
  hex[32] = 0;
  Serial.printf("[BOND] hasIrk=%d id=%s irk=%s track=%d\n", (int)hasIrk_,
                identity_.c_str(), hasIrk_ ? hex : "(none)",
                gBleScan.trackOn() ? 1 : 0);
  // 已知配对期 RPA：离线校验 resolveRpa 是否与 Bluedroid 一致
  const char* pairRpa = "74:6F:87:A1:9A:56";
  Serial.printf("[BOND] selftest pairRpa %s -> %d\n", pairRpa,
                (int)matchesAddr(pairRpa));
  int n = esp_ble_get_bond_device_num();
  Serial.printf("[BOND] system bond count=%d\n", n);
  if (n > 0) {
    if (n > 8) n = 8;
    esp_ble_bond_dev_t list[8];
    int cnt = n;
    if (esp_ble_get_bond_device_list(&cnt, list) == ESP_OK) {
      for (int i = 0; i < cnt; i++) {
        Serial.printf("[BOND] sys[%d] %s mask=0x%x\n", i,
                      macFromNative(list[i].bd_addr).c_str(),
                      (unsigned)list[i].bond_key.key_mask);
        if (list[i].bond_key.key_mask & ESP_BLE_ID_KEY_MASK) {
          const auto& pid = list[i].bond_key.pid_key;
          char ph[33];
          for (int j = 0; j < 16; j++)
            snprintf(ph + j * 2, 3, "%02x", pid.irk[j]);
          ph[32] = 0;
          Serial.printf("[BOND] sys[%d] irk=%s static=%s\n", i, ph,
                        macFromNative(pid.static_addr).c_str());
        }
      }
    }
  }
}

#else

BleBond gBleBond;
void BleBond::begin() {}
void BleBond::requestOpenPairing(uint32_t) {}
void BleBond::openPairingWindow(uint32_t) {}
void BleBond::closePairingWindow(const char*) {}
bool BleBond::pairingOpen() const { return false; }
void BleBond::service() {}
bool BleBond::matchesAddr(const String&) const { return false; }
bool BleBond::savePeerIdKey(const uint8_t*, const uint8_t*) { return false; }
void BleBond::clearBond(const char*) {}
void BleBond::loadFromStore() {}
void BleBond::startAdv() {}
void BleBond::stopAdv() {}
void BleBond::disconnectAll() {}
void BleBond::clearSystemBonds() {}
void BleBond::doOpen(uint32_t) {}
void BleBond::setPairingPin(const String&) {}
void BleBond::applyBleSecurity() {}
void BleBond::setAdvConnectable(bool) {}
uint32_t BleBond::staticPasskey() const { return 0; }
bool BleBond::trySaveFromSystemBond(const uint8_t*) { return false; }
void BleBond::requestDelayedClose(const char*, uint32_t) {}
bool BleBond::hasPasskey() const { return false; }
bool BleBond::allowSmp() const { return false; }
void BleBond::debugDump() {}

#endif
