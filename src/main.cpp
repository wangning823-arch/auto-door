#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include "config.h"
#include "ble_tracker.h"
#include "door_fsm.h"
#include "config_store.h"
#include "web_portal.h"
#include "ble_scan.h"
#include "rf_capture.h"
#include "nfc_reader.h"
#include "default_rf_keys.h"
#include "ble_bond.h"

// ===== 车库门智能控制器 P0.1 =====
// SoftAP 网页配置车机 MAC + F0/F1a/F2a/F3
// 手机连热点 GarageDoor-xxxx / 12345678 → 浏览器打开 192.168.4.1

static BleTracker gBt;
static DoorFsm gDoor;
static ConfigStore gCfg;
static WebPortal gWeb;
BleScanTool gBleScan;
static RfCapture gRf;
static NfcReader gNfc;
static char gMac[24] = CAR_BT_MAC;
static bool gBtStackInited = false;

// 经典 BT + BLE 配对栈：SoftAP 调试时推迟，优先让网页先出来
static void initBtStacks() {
  if (gBtStackInited) return;
  gBtStackInited = true;
  Serial.printf("[BT] init stacks t=%ums mac=%s\n", (unsigned)millis(), gMac);
  if (!gBt.begin(gMac)) {
    Serial.println("[BOOT] Classic BT init failed");
  }
  {
    int tm = gWeb.trackMode();
    bool classic = (tm == TRACK_MODE_CLASSIC);
    bool autoOn = gCfg.loadAutoTrack(classic);
    if (classic && gBt.hasTarget()) autoOn = true;
    gBt.setAutoTrack(autoOn);
    Serial.println("[BOOT] trackMode=" + String(classic ? "CLASSIC" : "BLE") +
                   " autotrack=" + String(autoOn ? "ON" : "OFF"));
  }
  Serial.println("[BOOT] BLE bond/IRK init...");
  gBleBond.begin();
  if (gBleBond.hasIrk()) {
    gBleScan.setTrack(true);
    Serial.println("[BOOT] IRK track ON (paired phone)");
  }
}

static void serviceBtStackInit() {
  if (gBtStackInited) return;
  // 热点打开期间一律不初始化 BT/BLE：
  // Bluedroid 起栈会拖垮 SoftAP 的 DHCP/HTTP（SSID 能连、网页永远打不开）
  if (gWeb.apActive()) return;
  initBtStacks();
}

static bool rfSaveKeyCb(int idx, const char* csv) {
  return gCfg.saveRfKey(idx, csv);
}

static bool rfEmitDoor(bool open) {
  int idx = open ? RF_KEY_OPEN : RF_KEY_CLOSE;
  if (!gRf.keyValid(idx)) return false;
  return gRf.playKey(idx);
}

// 本机是否已「进库/开过门」——只有成立后才允许自动关，避免上电/库内唤醒闪断连发 close
static bool gCloseArmed = false;

static bool autoCloseGuarded(const char* why) {
  if (millis() < AUTO_BOOT_GRACE_MS) {
    Serial.printf("[FSM] 关门跳过（上电宽限 %us 内）(%s)\n",
                  (unsigned)(AUTO_BOOT_GRACE_MS / 1000), why ? why : "");
    return false;
  }
  if (!gCloseArmed) {
    Serial.printf("[FSM] 关门跳过（未确认进库/开过门）(%s)\n", why ? why : "");
    return false;
  }
  return gDoor.tryAutoClose(why);
}

static bool autoOpenThenArm(const char* why) {
  bool ok = gDoor.tryAutoOpen(why);
  if (ok) gCloseArmed = true;
  return ok;
}

static int rfKeyIndexFromArg(const String& s) {
  String t = s;
  t.trim();
  t.toLowerCase();
  if (t == "0" || t == "open" || t == "up" || t == "开" || t == "上") return RF_KEY_OPEN;
  if (t == "1" || t == "close" || t == "down" || t == "关" || t == "下") return RF_KEY_CLOSE;
  if (t == "2" || t == "stop" || t == "pause" || t == "暂停" || t == "停") return RF_KEY_STOP;
  if (t == "3" || t == "lock" || t == "锁定" || t == "锁") return RF_KEY_LOCK;
  if (t.length() == 1 && t[0] >= '0' && t[0] <= '3') return t[0] - '0';
  return -1;
}

static void rfPrintKeys() {
  static const char* names[4] = {"open/up", "close/down", "stop/pause", "lock"};
  for (int i = 0; i < RF_KEY_COUNT; i++) {
    Serial.printf("[RF] key %d (%s): %s, %u pulses\n", i, names[i],
                  gRf.keyValid(i) ? "OK" : "empty", gRf.keyCount(i));
  }
}

// rfauto：周期自动发开门码。
// 注意：rfcap 开头不再强制发射——否则会和「按真实遥控」抢窗口、也容易 1s 内收尾。
static bool gRfAutoTx = false;
static uint32_t gRfAutoNextMs = 0;
static const uint32_t RF_AUTO_INTERVAL_MS = 5000;

static void rfAutoTxFire(const char* why) {
  if (!gRfAutoTx) return;
  if (!gRf.keyValid(RF_KEY_OPEN)) {
    Serial.println("[RF] AUTO TX 失败：key0 未学习");
    return;
  }
  gRfAutoNextMs = millis() + RF_AUTO_INTERVAL_MS;
  Serial.printf("[RF] AUTO TX open (%s) GPIO26...\n", why);
  gRf.playKey(RF_KEY_OPEN);
}

static void rfAutoTxService() {
  if (!gRfAutoTx) return;
  if (!millisReached(millis(), gRfAutoNextMs)) return;
  rfAutoTxFire("interval");
}

// 运行中长按 BOOT(GPIO0) 3s：强制开 SoftAP
// 注意：不能在「上电时按住」——GPIO0 会进 ROM 下载模式，应用根本不会跑
static bool gForceApArmed = false;
static uint32_t gBootHoldStartMs = 0;
static bool gForceApHandled = false;

static void serviceBootLongPress() {
  bool pressed = digitalRead(PIN_LEARN_BTN) == LOW;
  uint32_t now = millis();

  if (pressed) {
    if (!gForceApArmed) {
      gForceApArmed = true;
      gBootHoldStartMs = now;
      gForceApHandled = false;
      Serial.println("[BOOT] BOOT pressed — hold 3s to force SoftAP...");
    } else if (!gForceApHandled && (now - gBootHoldStartMs) >= 3000) {
      gForceApHandled = true;
      digitalWrite(PIN_STATUS_LED, HIGH);
      delay(80);
      digitalWrite(PIN_STATUS_LED, LOW);
      delay(80);
      digitalWrite(PIN_STATUS_LED, HIGH);
      delay(80);
      digitalWrite(PIN_STATUS_LED, LOW);

      gCfg.saveWifiEnabled(true);
      if (!gWeb.apActive()) {
        gWeb.startAp();
      }
      Serial.println("[BOOT] force SoftAP ON -> " + gWeb.apSsid() +
                     " pass=" + AP_PASSWORD);
      Serial.println("[BOOT] 手机连热点后打开 http://192.168.4.1/");
    }
  } else {
    if (gForceApArmed && !gForceApHandled && (now - gBootHoldStartMs) >= 1500 &&
        (now - gBootHoldStartMs) < 3000) {
      Serial.println("[BOOT] BOOT released too early (<3s), skip force AP");
    }
    gForceApArmed = false;
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() == 0) {
        line = "";
        return;
      }
      line.trim();
      if (line == "status") {
        Serial.printf("[CMD] %s | %s | mac=%s ap=%s ip=%s sta=%d bt=%d rfauto=%s\n",
                      gDoor.debugLine().c_str(), gBt.debugLine().c_str(), gMac,
                      gWeb.apSsid().c_str(), WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum(), (int)gBtStackInited,
                      gRfAutoTx ? "ON" : "OFF");
      } else if (line == "open" || line == "close") {
        gDoor.requestManualToggle(OpenSource::NFC);
      } else if (line == "hold on") {
        gDoor.setHoldOpen(true);
        Serial.println("[CMD] holdOpen=1");
      } else if (line == "hold off") {
        gDoor.setHoldOpen(false);
        Serial.println("[CMD] holdOpen=0");
      } else if (line.startsWith("mac ")) {
        String m = line.substring(4);
        m.trim();
        m.toUpperCase();
        m.toCharArray(gMac, sizeof(gMac));
        gCfg.saveMac(m);
        gBt.begin(gMac);
        Serial.println("[CMD] MAC set+saved " + m);
      } else if (line == "wifi") {
        Serial.println("[CMD] AP " + gWeb.apSsid() +
                       (gWeb.apActive() ? " active" : " off") + " pass=" +
                       AP_PASSWORD);
        if (gWeb.apActive()) {
          Serial.println("[CMD] ip=" + WiFi.softAPIP().toString());
        }
      } else if (line == "wifi off") {
        gCfg.saveWifiEnabled(false);
        gWeb.stopAp();
        gBt.setInquiryPaused(false);
        Serial.println("[CMD] WiFi OFF + saved (BT inquiry free)");
        // 关热点后若尚未 init BT，立刻起栈，便于测自动门
        if (!gBtStackInited) initBtStacks();
      } else if (line == "wifi on") {
        gCfg.saveWifiEnabled(true);
        if (gWeb.startAp()) {
          Serial.println("[CMD] WiFi ON " + gWeb.apSsid() + " " +
                         WiFi.softAPIP().toString());
        } else {
          Serial.println("[CMD] WiFi ON failed");
        }
      } else if (line == "wifi restart") {
        Serial.println("[CMD] WiFi restart SoftAP+HTTP...");
        gWeb.stopAp();
        delay(200);
        bool ok2 = gWeb.startAp();
        Serial.printf("[CMD] wifi restart -> %s ip=%s sta=%d\n",
                      ok2 ? "OK" : "FAIL",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
      } else if (line == "wifi status") {
        Serial.printf("[CMD] mode=%d ap=%s ip=%s sta=%d apmac=%s heap=%u bt=%d\n",
                      (int)WiFi.getMode(), gWeb.apSsid().c_str(),
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum(),
                      WiFi.softAPmacAddress().c_str(),
                      (unsigned)ESP.getFreeHeap(), (int)gBtStackInited);
      } else if (line == "relay high" || line == "relay low" || line == "relay pulse") {
        int pin = gDoor.relayPin();
        if (line == "relay high") {
          digitalWrite(pin, HIGH);
          Serial.printf("[CMD] GPIO%d=HIGH (%s)\n", pin,
                        RELAY_ACTIVE_LOW ? "release" : "energize");
        } else if (line == "relay low") {
          digitalWrite(pin, LOW);
          Serial.printf("[CMD] GPIO%d=LOW (%s)\n", pin,
                        RELAY_ACTIVE_LOW ? "energize" : "release");
        } else {
          gDoor.requestManualToggle(OpenSource::NFC);
        }
      } else if (line.startsWith("pin ")) {
        int p = atoi(line.substring(4).c_str());
        if (p > 0) {
          gDoor.setRelayPin(p);
          Serial.println("[CMD] move white wire to GPIO" + String(p));
        } else {
          Serial.println("[CMD] usage: pin 21|13|32|33|26");
        }
      } else if (line == "blink") {
        Serial.println("[CMD] blink GPIO" + String(gDoor.relayPin()) + " x8");
        for (int i = 0; i < 8; i++) {
          digitalWrite(gDoor.relayPin(), HIGH);
          delay(500);
          digitalWrite(gDoor.relayPin(), LOW);
          delay(500);
        }
        // 恢复空闲：高电平触发=低；低电平触发=高
#if RELAY_ACTIVE_LOW
        digitalWrite(gDoor.relayPin(), HIGH);
#else
        digitalWrite(gDoor.relayPin(), LOW);
#endif
        Serial.println("[CMD] blink done (idle)");
      } else if (line == "led") {
        // 自检：只闪板载 LED，证明串口+固件在跑
        Serial.println("[CMD] board LED GPIO2 blink x6");
        pinMode(PIN_STATUS_LED, OUTPUT);
        for (int i = 0; i < 6; i++) {
          digitalWrite(PIN_STATUS_LED, HIGH);
          delay(300);
          digitalWrite(PIN_STATUS_LED, LOW);
          delay(300);
        }
        Serial.println("[CMD] led done");
      } else if (line == "ble" || line.startsWith("ble ")) {
        // ble | ble 12 → 主动扫 10/12 秒，打印名称/UUID/厂商/RSSI
        uint32_t ms = 10000;
        int sp = line.indexOf(' ');
        if (sp > 0) {
          int sec = atoi(line.substring(sp + 1).c_str());
          if (sec >= 5 && sec <= 30) ms = (uint32_t)sec * 1000;
        }
        gBt.setAutoTrack(false);
        gBt.setInquiryPaused(true);
        Serial.println("[CMD] classic inquiry paused; BLE scan starting...");
        gBleScan.runScan(ms);
      } else if (line == "bletrack on") {
        gBleScan.setTrack(true);
        Serial.println("[CMD] BLE IRK track ON (paired phone only)");
      } else if (line == "bletrack off") {
        gBleScan.setTrack(false);
        Serial.println("[CMD] BLE track OFF");
      } else if (line.startsWith("blefilter")) {
        Serial.println(
            "[CMD] 名称/MAC 特征通道已移除；请用手机配对 (blepair) 后 IRK 跟踪");
      } else if (line == "blebond") {
        Serial.printf(
            "[CMD] IRK=%s id=%s pair=%s pin=%s(%u) track=%s\n",
            gBleBond.hasIrk() ? "YES" : "NO",
            gBleBond.identityMac().c_str(),
            gBleBond.pairingOpen() ? "OPEN" : "CLOSED",
            gBleBond.hasPasskey() ? "YES" : "NO",
            (unsigned)gBleBond.pairingPin().length(),
            gBleScan.trackOn() ? "ON" : "OFF");
        if (gBleBond.hasPasskey()) {
          Serial.printf("[CMD] PIN value=%s\n", gBleBond.pairingPin().c_str());
        }
        gBleBond.debugDump();
      } else if (line == "blepair") {
        gBleBond.requestOpenPairing(90000);
      } else if (line.startsWith("blepair ")) {
        int sec = atoi(line.substring(8).c_str());
        if (sec <= 0)
          gBleBond.requestOpenPairing(0);
        else
          gBleBond.requestOpenPairing((uint32_t)sec * 1000);
      } else if (line == "blepair off") {
        gBleBond.closePairingWindow("manual");
      } else if (line == "bleunpair") {
        gBleBond.clearBond("serial");
      } else if (line.startsWith("blepin ")) {
        gBleBond.setPairingPin(line.substring(7));
      } else if (line == "autotrack on") {
        gBt.setAutoTrack(true);
        gCfg.saveAutoTrack(true);
        Serial.println("[CMD] autotrack ON (periodic inquiry, saved)");
      } else if (line == "autotrack off") {
        gBt.setAutoTrack(false);
        gCfg.saveAutoTrack(false);
        Serial.println("[CMD] autotrack OFF (saved)");
      } else if (line == "rfcap") {
        // 连续抓包：一直听，直到 rfstop / GUI 停止
        Serial.println("[RF] RFCAP_OK 进入连续抓包");
        gRf.captureContinuous();
      } else if (line == "rfstop") {
        RfCapture::requestStop();
        Serial.println("[RF] 收到 rfstop，正在结束连续抓包...");
      } else if (line == "rfdump") {
        gRf.dump(100);
      } else if (line.startsWith("rflearn ")) {
        int idx = rfKeyIndexFromArg(line.substring(8));
        if (idx < 0) {
          Serial.println("[RF] 用法: rflearn 0|1|2|3  或 open/close/stop/lock");
        } else if (gRf.learnKey(idx, rfSaveKeyCb)) {
          Serial.printf("[RF] 按键 %d 学习成功，可用 rfplay %d 测试\n", idx, idx);
          rfPrintKeys();
        }
      } else if (line.startsWith("rfplay ")) {
        int idx = rfKeyIndexFromArg(line.substring(7));
        if (idx < 0) {
          Serial.println("[RF] 用法: rfplay 0|1|2|3  或 open/close/stop/lock");
        } else {
          gRf.playKey(idx);
        }
      } else if (line.startsWith("rfloop")) {
        // rfloop / rfloop 0 / rfloop 0 3
        int idx = RF_KEY_OPEN;
        uint8_t reps = 2;
        String rest = line.substring(6);
        rest.trim();
        if (rest.length()) {
          int sp = rest.indexOf(' ');
          if (sp > 0) {
            idx = rfKeyIndexFromArg(rest.substring(0, sp));
            int r = rest.substring(sp + 1).toInt();
            if (r >= 1 && r <= 6) reps = (uint8_t)r;
          } else {
            idx = rfKeyIndexFromArg(rest);
          }
        }
        if (idx < 0) {
          Serial.println("[RF] 用法: rfloop [0-3] [repeats]  例: rfloop 0 2");
        } else {
          gRf.loopbackKey(idx, reps);
        }
      } else if (line.startsWith("rfauto")) {
        // rfauto / rfauto on / rfauto off  （写入 NVS，重启仍保持）
        String rest = line.substring(6);
        rest.trim();
        rest.toLowerCase();
        if (rest == "off" || rest == "0") {
          gRfAutoTx = false;
          gCfg.saveRfAuto(false);
          Serial.println("[RF] rfauto OFF（已保存）");
        } else {
          gRfAutoTx = true;
          gRfAutoNextMs = millis();
          gCfg.saveRfAuto(true);
          Serial.printf("[RF] rfauto ON：每 %ums 发 key0(open)，已写入 NVS\n",
                        (unsigned)RF_AUTO_INTERVAL_MS);
          rfAutoTxFire("rfauto-on");
        }
      } else if (line.startsWith("rfbench")) {
        // rfbench / rfbench 0 / rfbench 0 6
        int idx = RF_KEY_OPEN;
        uint8_t rounds = 6;
        String rest = line.substring(7);
        rest.trim();
        if (rest.length()) {
          int sp = rest.indexOf(' ');
          if (sp > 0) {
            idx = rfKeyIndexFromArg(rest.substring(0, sp));
            int r = rest.substring(sp + 1).toInt();
            if (r >= 1 && r <= 20) rounds = (uint8_t)r;
          } else {
            idx = rfKeyIndexFromArg(rest);
          }
        }
        if (idx < 0) {
          Serial.println("[RF] 用法: rfbench [0-3] [rounds]  例: rfbench 0 6（每10s发一次）");
        } else {
          gRf.benchLoopbackKey(idx, rounds, 10000);
        }
      } else if (line.startsWith("rfcloop")) {
        // rfcloop / rfcloop 800
        uint32_t ms = 800;
        int sp = line.indexOf(' ');
        if (sp > 0) {
          long v = line.substring(sp + 1).toInt();
          if (v >= 100 && v <= 3000) ms = (uint32_t)v;
        }
        gRf.carrierLoopback(ms);
      } else if (line.startsWith("rfcarrier")) {
        // rfcarrier / rfcarrier 2000
        uint32_t ms = 1000;
        int sp = line.indexOf(' ');
        if (sp > 0) {
          long v = line.substring(sp + 1).toInt();
          if (v >= 50 && v <= 5000) ms = (uint32_t)v;
        }
        gRf.carrierTest(ms);
      } else if (line.startsWith("rfset ")) {
        // rfset 0 123,456,... 手动灌码
        String rest = line.substring(6);
        int sp = rest.indexOf(' ');
        if (sp > 0) {
          int idx = rfKeyIndexFromArg(rest.substring(0, sp));
          String csv = rest.substring(sp + 1);
          csv.trim();
          if (idx >= 0 && csv.length()) {
            if (gRf.setKeyFromCsv(idx, csv.c_str())) {
              gCfg.saveRfKey(idx, csv.c_str());
              Serial.printf("[RF] key %d 已写入 (%u pulses)\n", idx, gRf.keyCount(idx));
            } else {
              Serial.println("[RF] CSV 解析失败");
            }
          }
        }
      } else if (line.startsWith("rfclear")) {
        for (int i = 0; i < RF_KEY_COUNT; i++) {
          gCfg.clearRfKey(i);
          gRf.setKeyFromCsv(i, "");
        }
        Serial.println("[RF] 已清除全部按键");
        rfPrintKeys();
      } else if (line == "rfkeys") {
        rfPrintKeys();
      } else if (line == "rfexport") {
        for (int i = 0; i < RF_KEY_COUNT; i++) gRf.exportKeyCsv(i);
        Serial.println("[RF] export done");
      } else if (line == "gpio17" || line == "i2cscan" || line == "i2cscan2") {
        // 推拉测试只给 gpio17：i2cscan 前不要动 SCL，否则会把 PN532 弄挂
        if (line == "gpio17") {
          pinMode(PIN_NFC_SCL, OUTPUT);
          digitalWrite(PIN_NFC_SCL, HIGH);
          delay(2);
          int driven = digitalRead(PIN_NFC_SCL);
          digitalWrite(PIN_NFC_SCL, LOW);
          delay(2);
          int drivenLow = digitalRead(PIN_NFC_SCL);
          pinMode(PIN_NFC_SCL, INPUT_PULLUP);
          delay(5);
          int released = digitalRead(PIN_NFC_SCL);
          pinMode(PIN_NFC_SDA, INPUT_PULLUP);
          delay(2);
          int sda = digitalRead(PIN_NFC_SDA);
          Serial.printf(
              "[GPIO] SCL17 driveH=%d driveL=%d release_pullup=%d | SDA16=%d\n",
              driven, drivenLow, released, sda);
          if (driven == 1 && released == 0) {
            Serial.println("[GPIO] 结论: 能推高但松开后为0 → 外部器件拉住 SCL");
          } else if (driven == 0) {
            Serial.println("[GPIO] 结论: 推高仍为0 → SCL 对地硬短路");
          } else if (released == 1) {
            Serial.println("[GPIO] 结论: 松开后为1 → 总线空闲正常");
          }
        } else {
        bool multi = (line == "i2cscan2");
        static const int pairs[][2] = {
            {PIN_NFC_SDA, PIN_NFC_SCL}, {4, 15},  {18, 19}, {32, 33},
            {12, 14},     {13, 32},     {23, 22}, {2, 15},
        };
        int nPairs = multi ? (int)(sizeof(pairs) / sizeof(pairs[0])) : 1;
        int total = 0;
        for (int p = 0; p < nPairs; p++) {
          int sdaP = pairs[p][0], sclP = pairs[p][1];
          if (sdaP == PIN_RELAY || sclP == PIN_RELAY || sdaP == PIN_RF_DATA ||
              sclP == PIN_RF_DATA || sdaP == PIN_RF_TX || sclP == PIN_RF_TX) {
            continue;
          }
          Wire.end();
          pinMode(sdaP, INPUT_PULLUP);
          pinMode(sclP, INPUT_PULLUP);
          Wire.begin(sdaP, sclP);
          Wire.setTimeOut(50);
          int lvlSda = digitalRead(sdaP), lvlScl = digitalRead(sclP);
          int found = 0;
          uint8_t codes[3] = {255, 255, 255};
          int nTry = 0;
          for (uint8_t a = 0x08; a <= 0x77; a++) {
            Wire.beginTransmission(a);
            uint8_t e = Wire.endTransmission();
            if (nTry < 3) codes[nTry++] = e;
            if (e == 0) {
              Serial.printf("[I2C] FOUND sda=%d scl=%d addr=0x%02X\n", sdaP,
                            sclP, a);
              found++;
            }
            if (e == 5) break;
          }
          Serial.printf(
              "[I2C] pair sda=%d scl=%d lvl=%d/%d found=%d e0=%u e1=%u e2=%u\n",
              sdaP, sclP, lvlSda, lvlScl, found, codes[0], codes[1], codes[2]);
          total += found;
        }
        Serial.printf("[I2C] multi-scan done total=%d\n", total);
        Wire.end();
        Wire.begin(PIN_NFC_SDA, PIN_NFC_SCL);
        // 不调用 gNfc.begin()：会清掉已 PN532 ready 的状态
        }
      } else if (line == "sclhold") {
        gNfc.holdSclHigh();
      } else if (line == "sclrelease") {
        gNfc.releaseScl();
      } else if (line == "buspull") {
        pinMode(PIN_NFC_SDA, INPUT_PULLUP);
        pinMode(PIN_NFC_SCL, INPUT_PULLUP);
        Serial.printf("[BUS] buspull SDA16=%d SCL17=%d\n",
                      digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL));
      } else if (line == "busfree") {
        // 只松 Wire，不碰 PN532 命令（Error263 后 SCL 卡 0.04 时用）
        Wire.end();
        pinMode(PIN_NFC_SDA, INPUT_PULLUP);
        pinMode(PIN_NFC_SCL, INPUT_PULLUP);
        Serial.printf("[BUS] busfree Wire.end SDA16=%d SCL17=%d\n",
                      digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL));
      } else if (line == "nfcinit") {
        Serial.println("[CMD] nfcinit 强制重新初始化...");
        if (gNfc.forceInit()) {
          Serial.println("[CMD] nfcinit OK");
        } else {
          Serial.println("[CMD] nfcinit FAIL");
        }
      } else if (line == "nfcscan") {
        Serial.println("[CMD] NFC 持续监听已开，贴卡（最多 30 秒）...");
        if (!gNfc.ok()) gNfc.forceInit();
        gNfc.startListen(0);  // 持续
        String uid;
        uint32_t t0 = millis();
        while (millis() - t0 < 30000) {
          if (gNfc.poll(uid)) {
            Serial.println("[NFC] 读到卡: " + uid);
            break;
          }
          delay(20);
        }
        if (uid.length() == 0) Serial.println("[NFC] 超时未读到卡");
        Serial.printf("[NFC] scan end SCL17=%d\n",
                      digitalRead(PIN_NFC_SCL));
      } else if (line.startsWith("nfcsave ")) {
        String uid = line.substring(8);
        uid.trim();
        uid.toUpperCase();
        if (uid.length() >= 8) {
          gCfg.saveNfcUid(uid);
          gNfc.setAuthUid(uid);
          Serial.println("[NFC] 已保存授权卡: " + uid);
        } else {
          Serial.println("[NFC] UID 太短，先 nfcscan 读卡");
        }
      } else if (line == "nfcclear") {
        gCfg.clearNfcUid();
        gNfc.setAuthUid("");
        Serial.println("[NFC] 已清除授权卡");
      } else if (line == "help") {
        Serial.println(
            "cmds: status | open | close | rfcap | rfstop | rflearn 0-3 | rfplay 0-3 | rfauto on|off | rfloop 0 | rfbench 0 6 | rfcloop | rfcarrier | rfkeys | "
            "rfexport | rfclear | rfdefaults | i2cscan | i2cscan2 | buspull | busfree | sclhold | sclrelease | nfcscan | nfcsave <uid> | wifi on|off | autotrack on|off | blebond | blepair [sec] | bleunpair");
      } else if (line == "rfdefaults") {
        // 强制写入实车验证的开/关码（修第二块板开码不对）
        bool ok0 = gRf.setKeyFromCsv(RF_KEY_OPEN, RF_DEFAULT_OPEN_CSV);
        bool ok1 = gRf.setKeyFromCsv(RF_KEY_CLOSE, RF_DEFAULT_CLOSE_CSV);
        if (ok0) gCfg.saveRfKey(RF_KEY_OPEN, RF_DEFAULT_OPEN_CSV);
        if (ok1) gCfg.saveRfKey(RF_KEY_CLOSE, RF_DEFAULT_CLOSE_CSV);
        Serial.printf("[RF] 默认码已写入 open=%d close=%d\n", ok0, ok1);
        rfPrintKeys();
        if (ok0) gRf.exportKeyCsv(RF_KEY_OPEN);
        if (ok1) gRf.exportKeyCsv(RF_KEY_CLOSE);
      } else {
        Serial.println("[CMD] unknown, try help");
      }
      line = "";
    } else {
      if (line.length() < 1024) line += c;
    }
  }
}

void setup() {
  Serial.begin(SerialBaud);
  delay(200);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" Garage Door Controller  P0.1");
  Serial.println(" SoftAP web config + gradual BT");
  Serial.println("========================================");

  // 最早期测 SDA/SCL 电平（尚未碰 I2C/WiFi/BT）——排除软件把脚拉死
  pinMode(PIN_NFC_SDA, INPUT_PULLUP);
  pinMode(PIN_NFC_SCL, INPUT_PULLUP);
  delay(2);
  Serial.printf("[BOOT] early SDA16=%d SCL17=%d t=%ums\n",
                digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL),
                (unsigned)millis());

  gDoor.begin();
  gCfg.begin();
  gRf.begin(PIN_RF_DATA, PIN_RF_TX);
  gRf.setIdleHook(rfAutoTxService);
  gDoor.setRfEmit(rfEmitDoor);

  // 开关码统一：key0/key1 每次上电强制为实车验证默认码（多板一致）。
  // key2/key3（暂停/锁）仍从 NVS 加载。rflearn 0/1 重启后会被默认码覆盖。
  {
    for (int i = 2; i < RF_KEY_COUNT; i++) {
      String csv = gCfg.loadRfKey(i);
      if (csv.length()) gRf.setKeyFromCsv(i, csv.c_str());
    }

    String nvs0 = gCfg.loadRfKey(RF_KEY_OPEN);
    String nvs1 = gCfg.loadRfKey(RF_KEY_CLOSE);
    bool changed = (nvs0 != RF_DEFAULT_OPEN_CSV) || (nvs1 != RF_DEFAULT_CLOSE_CSV);

    bool ok0 = gRf.setKeyFromCsv(RF_KEY_OPEN, RF_DEFAULT_OPEN_CSV);
    bool ok1 = gRf.setKeyFromCsv(RF_KEY_CLOSE, RF_DEFAULT_CLOSE_CSV);
    if (ok0 && ok1) {
      if (changed) {
        gCfg.saveRfKey(RF_KEY_OPEN, RF_DEFAULT_OPEN_CSV);
        gCfg.saveRfKey(RF_KEY_CLOSE, RF_DEFAULT_CLOSE_CSV);
        Serial.println("[BOOT] 开关码已统一为默认开49p/关80p（NVS 已更新）");
      } else {
        Serial.println("[BOOT] 开关码已是统一默认开49p/关80p");
      }
    } else {
      Serial.println("[BOOT] 默认开关码写入失败");
    }

    Serial.println("[BOOT] RF keys:");
    rfPrintKeys();
    for (int i = 0; i < RF_KEY_COUNT; i++) {
      if (gRf.keyValid(i)) gRf.exportKeyCsv(i);
    }
  }

  // 上电恢复 rfauto（防止 RF.bat 连串口复位后丢掉周期发射）
  gRfAutoTx = gCfg.loadRfAuto(false);
  if (gRfAutoTx) {
    gRfAutoNextMs = millis() + 1000;
    Serial.println("[BOOT] rfauto=ON（NVS），每 5s 自动发 key0");
  } else {
    Serial.println("[BOOT] rfauto=OFF，串口: rfauto on 可开启并保存");
  }

  // SoftAP/HTTP 必须先起；NFC 绝不能挡启动
  String saved = gCfg.loadMac(CAR_BT_MAC);
  saved.toCharArray(gMac, sizeof(gMac));
  Serial.println("[BOOT] car MAC from NVS: " + saved);

  bool wifiOn = gCfg.loadWifiEnabled(true);
#if WIFI_DEBUG_BOOT_ON
  // 调试：上电一律开热点；网页关 WiFi 只影响本次，断电/复位后自动回来
  if (!wifiOn) {
    wifiOn = true;
    Serial.println("[BOOT] WIFI_DEBUG_BOOT_ON=1 → 忽略 NVS wifi_on=0，强制开 SoftAP");
  }
#endif
  Serial.println("[BOOT] wifiOn=" + String(wifiOn ? 1 : 0) +
                 " debug_boot=" + String(WIFI_DEBUG_BOOT_ON ? 1 : 0));

  // NFC：上电约 5s 后自动 init（原先永久 deferred，断电后刷卡会失效）
  Serial.println("[BOOT] NFC auto-init scheduled (~5s)");
  gNfc.begin(PIN_NFC_SDA, PIN_NFC_SCL);
  pinMode(PIN_NFC_SDA, INPUT_PULLUP);
  pinMode(PIN_NFC_SCL, INPUT_PULLUP);
  {
    String auth = gCfg.loadNfcUid();
    gNfc.setAuthUid(auth);
    Serial.println("[BOOT] NFC auth: " + (auth.length() ? auth : String("(未注册)")));
  }
  Serial.printf("[BOOT] after-nfc-begin t=%ums SCL17=%d\n", (unsigned)millis(),
                digitalRead(PIN_NFC_SCL));

  Serial.println("[BOOT] web/WiFi first...");
  Serial.printf("[BOOT] before-web t=%ums SCL17=%d\n", (unsigned)millis(),
                digitalRead(PIN_NFC_SCL));
  gWeb.begin(&gCfg, &gBt, &gDoor, &gBleScan, &gNfc, wifiOn);
  delay(300);
  Serial.printf("[BOOT] after-web t=%ums SCL17=%d\n", (unsigned)millis(),
                digitalRead(PIN_NFC_SCL));

  // SoftAP 调试：网页优先。BT/BLE 推迟；NFC 自动 init 避开「有人连热点」时
  // （不在此长期 postpone：无人连热点时 T+5s 仍可 init，保证之后 NFC+蓝牙能并存）
  if (wifiOn) {
    Serial.println("[BOOT] SoftAP on → BT/BLE 栈推迟到关热点后；NFC 空闲时自动 init");
  } else {
    initBtStacks();
  }

  Serial.println("[BOOT] ready. wifi=" + String(wifiOn ? 1 : 0) +
                 " bt_inited=" + String(gBtStackInited ? 1 : 0));
  Serial.printf("[BOOT] ready t=%ums SDA16=%d SCL17=%d\n", (unsigned)millis(),
                digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL));
  if (wifiOn) {
    Serial.println("[BOOT] 手机WiFi连接: " + gWeb.apSsid() + "  密码: " + AP_PASSWORD);
    Serial.println("[BOOT] 浏览器打开: http://192.168.4.1/");
    Serial.println("[BOOT] 注意：热点打开期间不初始化蓝牙栈（保证 DHCP/网页）");
    Serial.println("[BOOT] 测自动门：网页关 WiFi 或串口 wifi off 后才会 init BT");
#if WIFI_DEBUG_BOOT_ON
    Serial.println("[BOOT] WiFi=调试模式：重新上电会自动再开热点");
#endif
  } else {
    Serial.println("[BOOT] SoftAP off. 运行中长按 BOOT 3 秒（LED 闪两下）可强制开热点");
    Serial.println("[BOOT] 或串口发 wifi on");
  }

  // WiFi 事件：关联/拿 IP 与网页打不开时对照
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    switch (event) {
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        Serial.printf("[WiFi] STA 已关联 clients=%d\n",
                      WiFi.softAPgetStationNum());
        break;
      case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        Serial.println("[WiFi] STA 已分配 IP（应能打开 192.168.4.1）");
        break;
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        Serial.printf("[WiFi] STA 断开 clients=%d\n",
                      WiFi.softAPgetStationNum());
        break;
      default:
        break;
    }
  });
  Serial.println("[BOOT] 蓝牙扫描用的是经典蓝牙 Inquiry（车机需开启「可被搜索」）");
}

void loop() {
  // SCL 掉压监视：边沿必打；热点调试期降低稳态心跳频率，少占串口/loop
  {
    static int lastSda = -1, lastScl = -1;
    static uint32_t lastBusLog = 0;
    int sda = digitalRead(PIN_NFC_SDA);
    int scl = digitalRead(PIN_NFC_SCL);
    uint32_t now = millis();
    uint32_t busPeriod = gWeb.apActive() ? 5000 : 2000;
    if (sda != lastSda || scl != lastScl) {
      Serial.printf("[BUS] t=%ums SDA16=%d SCL17=%d%s\n", (unsigned)now, sda,
                    scl, (scl ? " (idle high)" : " (SCL LOW)"));
      lastSda = sda;
      lastScl = scl;
      lastBusLog = now;
    } else if (now - lastBusLog >= busPeriod) {
      Serial.printf("[BUS] t=%ums SDA16=%d SCL17=%d\n", (unsigned)now, sda,
                    scl);
      lastBusLog = now;
    }
  }

  gWeb.loop();
  gDoor.loop(gBt);
  serviceBootLongPress();
  handleSerial();
  rfAutoTxService();
  // 先跑蓝牙调度，再跑 NFC：避免 I2C 抢在 Inquiry/BLE 之前占满 loop
  serviceBtStackInit();
  if (gBtStackInited) {
    gBt.loop();
    gBleBond.service();
  }

  // ===== NFC 与 WiFi/蓝牙的共存策略 =====
  // 射频层：NFC=13.56MHz，BT=2.4GHz，互不干扰。
  // 软件层：PN532 poll/hwInit 会阻塞 loop，会拖慢网页和 Inquiry → 按场景调度。
  //  · SoftAP 有客户端：暂停刷卡轮询，保证网页（可接受的二选一）
  //  · 无网/热点无人：NFC + 蓝牙必须同时工作；NFC 降频，且不在 Inquiry 忙时做 hwInit
  {
    const bool apOn = gWeb.apActive();
    const bool apClient = apOn && WiFi.softAPgetStationNum() > 0;
    const bool btTrack =
        gBtStackInited && (gBt.autoTrack() || gBleScan.trackOn());
    const bool btBusy = gBtStackInited && gBt.inquiryBusy();
    const bool bleBusy = gBtStackInited && gBleScan.busy();

    static bool nfcPausedForWeb = false;
    if (apClient != nfcPausedForWeb) {
      nfcPausedForWeb = apClient;
      if (apClient) {
        gNfc.setListen(false);
        Serial.println("[NFC] 暂停轮询（热点有客户端，网页优先；断开后恢复）");
      } else if (gNfc.ok()) {
        gNfc.setListen(true);
        Serial.println("[NFC] 恢复轮询");
      }
    }

    // 跟踪中：加大 NFC 间隔并缩短单次 I2C 超时，给蓝牙留 loop
    if (btTrack) {
      gNfc.setPollGapMs(1200);
    } else if (apOn) {
      gNfc.setPollGapMs(500);
    } else {
      gNfc.setPollGapMs(350);
    }

    // 热点开着但无人连：允许刷卡；关热点后也允许
    if (!apClient && gNfc.ok() && !gNfc.listen()) {
      gNfc.setListen(true);
    }

    // NFC 未就绪时：仅在 BT Inquiry/BLE 不忙时才走 recover/hwInit
    if (!gNfc.ok()) {
      if (!apClient && !btBusy && !bleBusy) {
        String uid0;
        gNfc.poll(uid0);  // 内部 maybeRecover / 上电自动 init
      }
    } else {
      String uid;
      if (gNfc.poll(uid)) {
        Serial.println("[NFC] card: " + uid);
        if (gNfc.isAuthorized(uid)) {
          Serial.println("[NFC] authorized -> toggle");
          gDoor.requestManualToggle(OpenSource::NFC);
        } else if (gNfc.authUid().length() == 0) {
          Serial.println("[NFC] 未注册卡，串口执行: nfcsave " + uid);
        } else {
          Serial.println("[NFC] 未授权卡");
        }
      }
    }
  }

  // ===== 跟踪模式分发 =====
  int trackMode = gWeb.trackMode();  // 实时从 WebPortal 读取（网页可改）
  if (trackMode == TRACK_MODE_BLE) {
    // BLE：无→有且<-80立刻开；无→有且≥-80不开（库内开关蓝牙突变）；离场≤-90关
    enum class BlePhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static BlePhase phase = BlePhase::WAIT_SIGNAL;
    static uint8_t leaveFarStreak = 0;

    // SoftAP 调试期 / 有客户端：阻塞式 BLE 扫描会让热点时有时无、网页半截
    // 注意：关联完成前 softAPgetStationNum() 往往还是 0，必须以「热点是否打开」为准
    const bool wifiClient = WiFi.softAPgetStationNum() > 0;
    const bool apYield = gWeb.wifiRfPriority() || gWeb.rfQuietActive();
    if (gBtStackInited && gBleScan.trackOn() && !wifiClient && !apYield) {
      // runScan() 是阻塞的：busy 边沿在同一轮 trackPoll 内完成，不能靠 busy 跨轮判断
      const uint32_t prevScanEnd = gBleScan.lastScanEndMs();
      gBleScan.trackPoll(BLE_TRACK_INTERVAL_MS, BLE_TRACK_SCAN_MS);
      const bool scanJustFinished = gBleScan.lastScanEndMs() != prevScanEnd;
      if (scanJustFinished) {
        int r = gBleScan.matchRssi();
        bool lost = gBleScan.lostCar();
        bool seen = gBleScan.lastMatchMs() != 0 &&
                    (millis() - gBleScan.lastMatchMs()) < BLE_SILENT_GAP_MS;
        bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
        bool isStrong = hasSignal && r >= RSSI_STRONG;
        bool isFar = hasSignal && r <= RSSI_FAR_CLOSE;  // ≈走出 10m
        // 无→有且≥-80：库内开关蓝牙等突变，不开门（弱出现 <-80 才开）
        bool suddenVeryStrong = hasSignal && r >= RSSI_SUDDEN_STRONG;

        const char* phaseName =
            phase == BlePhase::WAIT_SIGNAL ? "WAIT" :
            phase == BlePhase::APPEARING  ? "APPEAR" :
            phase == BlePhase::STRONG     ? "STRONG" : "LEAVE";

        Serial.printf("[BLE] rssi=%d seen=%d strong=%d far=%d lost=%d phase=%s\n",
                      r, (int)hasSignal, (int)isStrong, (int)isFar, (int)lost,
                      phaseName);

        switch (phase) {
          case BlePhase::WAIT_SIGNAL:
            leaveFarStreak = 0;
            if (hasSignal && !suddenVeryStrong) {
              // 无→有且 < -80：关门贴近约 -90 也立刻开，不等渐强
              Serial.printf("[FSM] 无→有 rssi=%d (<-80)，自动开\n", r);
              autoOpenThenArm("无→有");
              phase = BlePhase::APPEARING;
            } else if (suddenVeryStrong) {
              // 无→有且 ≥ -80：库内开关蓝牙类突变，不操作
              phase = BlePhase::STRONG;
              Serial.printf("[FSM] 无→强跳变 rssi=%d (≥-80)，不自动开（库内突变？）\n", r);
            }
            break;

          case BlePhase::APPEARING:
            leaveFarStreak = 0;
            if (isStrong) {
              autoOpenThenArm("无→有→强");
              phase = BlePhase::STRONG;
            } else if (!hasSignal || lost) {
              Serial.println("[FSM] 弱信号消失，回 WAIT");
              autoCloseGuarded("有→无(未进库)");
              phase = BlePhase::WAIT_SIGNAL;
            }
            break;

          case BlePhase::STRONG:
            leaveFarStreak = 0;
            if (!isStrong && hasSignal) {
              phase = BlePhase::LEAVING;
              gCloseArmed = true;  // 确认曾在库内变强后再变弱 → 允许随后关门
              Serial.println("[FSM] 强→弱，车开始离开");
            } else if (!hasSignal || lost) {
              Serial.println("[FSM] 强信号直接消失，尝试关门");
              autoCloseGuarded("强→无");
              phase = BlePhase::WAIT_SIGNAL;
            }
            break;

          case BlePhase::LEAVING:
            // 主路径：RSSI≤-90 连续 2 次 ≈ 走出约 10m，不等信号完全消失
            if (isFar) {
              leaveFarStreak++;
              Serial.printf("[FSM] 离场远信号 rssi=%d %u/%u\n", r,
                            (unsigned)leaveFarStreak,
                            (unsigned)BLE_CLOSE_FAR_SCANS);
              if (leaveFarStreak >= BLE_CLOSE_FAR_SCANS) {
                gCloseArmed = true;
                autoCloseGuarded("离场约10m");
                phase = BlePhase::WAIT_SIGNAL;
                leaveFarStreak = 0;
              }
            } else {
              leaveFarStreak = 0;
              if (!hasSignal || lost) {
                gCloseArmed = true;
                autoCloseGuarded("强→弱→无");
                phase = BlePhase::WAIT_SIGNAL;
              } else if (isStrong) {
                phase = BlePhase::STRONG;
                Serial.println("[FSM] 弱→强（回到门口），取消离开");
              }
            }
            break;
        }
      }
    } else {
      static uint32_t lastWifiSkipLog = 0;
      if (millis() - lastWifiSkipLog > 15000) {
        lastWifiSkipLog = millis();
        if (wifiClient)
          Serial.println("[BLE] track scan skipped (SoftAP client, keep web alive)");
        else if (apYield)
          Serial.println("[BLE] track scan skipped (SoftAP RF yield; 关热点后恢复自动门扫描)");
      }
    }
  } else {
    // Classic：与 BLE 同一套；仅在 seen/rssi 边沿评估，避免每 loop 空转
    enum class ClPhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static ClPhase clPhase = ClPhase::WAIT_SIGNAL;
    static uint32_t lastClLogMs = 0;
    static bool clPrevSeen = false;
    static int clPrevRssi = -999;
    static bool clInited = false;
    static uint8_t clFarStreak = 0;

    if (gBt.autoTrack() && gBt.hasTarget()) {
      int r = gBt.lastRssi();
      bool seen = gBt.seenRecently(BLE_SILENT_GAP_MS);
      bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
      bool isStrong = hasSignal && r >= RSSI_STRONG;
      bool isFar = hasSignal && r <= RSSI_FAR_CLOSE;
      bool suddenVeryStrong = hasSignal && r >= RSSI_SUDDEN_STRONG;
      bool lost = !seen;

      // 远信号连续计数：即使 rssi 变化 <5dB 也要评估（防漏掉 -90 边沿）
      bool farEdge = (isFar != (clPrevRssi > -999 && clPrevRssi <= RSSI_FAR_CLOSE &&
                                 clPrevRssi >= RSSI_APPEAR_MIN));
      bool edge = !clInited || (seen != clPrevSeen) ||
                  (seen && abs(r - clPrevRssi) >= 5) || farEdge ||
                  (clPhase == ClPhase::LEAVING && seen);
      if (!edge) {
        // 无边沿只打周期日志
      } else {
        clInited = true;
        clPrevSeen = seen;
        clPrevRssi = r;

        if (millis() - lastClLogMs >= 2000) {
          lastClLogMs = millis();
          const char* phaseName =
              clPhase == ClPhase::WAIT_SIGNAL ? "WAIT" :
              clPhase == ClPhase::APPEARING   ? "APPEAR" :
              clPhase == ClPhase::STRONG      ? "STRONG" : "LEAVE";
          Serial.printf("[CLASSIC] rssi=%d seen=%d strong=%d far=%d lost=%d phase=%s\n",
                        r, (int)hasSignal, (int)isStrong, (int)isFar, (int)lost,
                        phaseName);
        }

        switch (clPhase) {
          case ClPhase::WAIT_SIGNAL:
            clFarStreak = 0;
            if (hasSignal && !suddenVeryStrong) {
              Serial.printf("[FSM] C 无→有 rssi=%d (<-80)，自动开\n", r);
              autoOpenThenArm("经典无→有");
              clPhase = ClPhase::APPEARING;
            } else if (suddenVeryStrong) {
              clPhase = ClPhase::STRONG;
              Serial.printf("[FSM] C 无→强跳变 rssi=%d (≥-80)，不自动开（库内突变？）\n", r);
            }
            break;

          case ClPhase::APPEARING:
            clFarStreak = 0;
            if (isStrong) {
              autoOpenThenArm("经典无→有→强");
              clPhase = ClPhase::STRONG;
            } else if (lost) {
              Serial.println("[FSM] C 弱信号消失，回 WAIT");
              autoCloseGuarded("经典有→无(未进库)");
              clPhase = ClPhase::WAIT_SIGNAL;
            }
            break;

          case ClPhase::STRONG:
            clFarStreak = 0;
            if (!isStrong && hasSignal) {
              clPhase = ClPhase::LEAVING;
              gCloseArmed = true;
              Serial.println("[FSM] C 强→弱，开始离开");
            } else if (lost) {
              Serial.println("[FSM] C 强信号消失，尝试关门");
              autoCloseGuarded("经典强→无");
              clPhase = ClPhase::WAIT_SIGNAL;
            }
            break;

          case ClPhase::LEAVING:
            if (isFar) {
              clFarStreak++;
              Serial.printf("[FSM] C 离场远信号 rssi=%d %u/%u\n", r,
                            (unsigned)clFarStreak,
                            (unsigned)BLE_CLOSE_FAR_SCANS);
              if (clFarStreak >= BLE_CLOSE_FAR_SCANS) {
                gCloseArmed = true;
                autoCloseGuarded("经典离场约10m");
                clPhase = ClPhase::WAIT_SIGNAL;
                clFarStreak = 0;
              }
            } else {
              clFarStreak = 0;
              if (lost) {
                gCloseArmed = true;
                autoCloseGuarded("经典强→弱→无");
                clPhase = ClPhase::WAIT_SIGNAL;
              } else if (isStrong) {
                clPhase = ClPhase::STRONG;
                Serial.println("[FSM] C 弱→强，取消离开");
              }
            }
            break;
        }
      }
    }
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 8000) {
    lastLog = millis();
    Serial.println("[LOG] " + gDoor.debugLine() + " | " + gBt.debugLine() +
                   " | ble_rssi=" + String(gBleScan.matchRssi()) +
                   " bond=" + String(gBleBond.hasIrk() ? "Y" : "N") + " | " +
                   gNfc.debugLine());
  }
}
