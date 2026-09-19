#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "ble_tracker.h"
#include "door_fsm.h"
#include "config_store.h"
#include "web_portal.h"
#include "ble_scan.h"
#include "rf_capture.h"
#include "nfc_reader.h"
#include "default_rf_keys.h"

// ===== 车库门智能控制器 P0.1 =====
// SoftAP 网页配置车机 MAC + F0/F1a/F2a/F3
// 手机连热点 GarageDoor-xxxx / 12345678 → 浏览器打开 192.168.4.1

static BleTracker gBt;
static DoorFsm gDoor;
static ConfigStore gCfg;
static WebPortal gWeb;
static BleScanTool gBleScan;
static RfCapture gRf;
static NfcReader gNfc;
static char gMac[24] = CAR_BT_MAC;

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
  if (millis() < gRfAutoNextMs) return;
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
        Serial.printf("[CMD] %s | %s | mac=%s ap=%s | rfauto=%s\n",
                      gDoor.debugLine().c_str(), gBt.debugLine().c_str(), gMac,
                      gWeb.apSsid().c_str(), gRfAutoTx ? "ON" : "OFF");
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
      } else if (line == "wifi on") {
        gCfg.saveWifiEnabled(true);
        if (gWeb.startAp()) {
          Serial.println("[CMD] WiFi ON " + gWeb.apSsid() + " " +
                         WiFi.softAPIP().toString());
        } else {
          Serial.println("[CMD] WiFi ON failed");
        }
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
        Serial.println("[CMD] BLE track ON filter=" + gBleScan.filter());
      } else if (line == "bletrack off") {
        gBleScan.setTrack(false);
        Serial.println("[CMD] BLE track OFF");
      } else if (line.startsWith("blefilter ")) {
        String f = line.substring(10);
        f.trim();
        gBleScan.setFilter(f);
        gCfg.saveBleFilter(f);
        gBleScan.setTrack(f.length() > 0);
        Serial.println("[CMD] BLE filter=" + f);
      } else if (line == "autotrack on") {
        gBt.setAutoTrack(true);
        Serial.println("[CMD] autotrack ON (periodic inquiry)");
      } else if (line == "autotrack off") {
        gBt.setAutoTrack(false);
        Serial.println("[CMD] autotrack OFF");
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
      } else if (line == "nfcscan") {
        Serial.println("[CMD] 等待刷卡 5 秒...");
        String uid;
        uint32_t t0 = millis();
        while (millis() - t0 < 5000) {
          if (gNfc.poll(uid)) {
            Serial.println("[NFC] 读到卡: " + uid);
            break;
          }
          delay(50);
        }
        if (uid.length() == 0) Serial.println("[NFC] 超时未读到卡");
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
            "rfexport | rfclear | rfdefaults | nfcscan | nfcsave <uid> | wifi on|off | autotrack on|off");
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

  Serial.println("[BOOT] NFC init...");
  if (gNfc.begin(PIN_NFC_SDA, PIN_NFC_SCL)) {
    String auth = gCfg.loadNfcUid();
    gNfc.setAuthUid(auth);
    Serial.println("[BOOT] NFC auth: " + (auth.length() ? auth : String("(未注册)")));
  }

  String saved = gCfg.loadMac(CAR_BT_MAC);
  saved.toCharArray(gMac, sizeof(gMac));
  Serial.println("[BOOT] car MAC from NVS: " + saved);

  bool wifiOn = gCfg.loadWifiEnabled(true);
  Serial.println("[BOOT] wifiOn=" + String(wifiOn ? 1 : 0));

  // 顺序：先起 SoftAP/TCP-IP，再起 Classic BT
  // 否则易触发 assert failed: tcpip_send_msg_wait_sem (Invalid mbox) 死循环重启
  Serial.println("[BOOT] web/WiFi first...");
  gWeb.begin(&gCfg, &gBt, &gDoor, &gBleScan, wifiOn);
  delay(300);

  Serial.println("[BOOT] Classic BT second...");
  if (!gBt.begin(gMac)) {
    Serial.println("[BOOT] Classic BT init failed");
  }

  Serial.println("[BOOT] ready. wifi=" + String(wifiOn ? "ON" : "OFF") +
                 " autotrack=" + String(gBt.autoTrack() ? "ON" : "OFF"));
  if (wifiOn) {
    Serial.println("[BOOT] 手机WiFi连接: " + gWeb.apSsid() + "  密码: " + AP_PASSWORD);
    Serial.println("[BOOT] 浏览器打开: http://192.168.4.1/");
    Serial.println("[BOOT] 网页点「关闭 WiFi」后，无网模式 BT Inquiry 独占射频");
  } else {
    Serial.println("[BOOT] SoftAP off. 运行中长按 BOOT 3 秒（LED 闪两下）可强制开热点");
    Serial.println("[BOOT] 或串口发 wifi on");
  }
  Serial.println("[BOOT] 蓝牙扫描用的是经典蓝牙 Inquiry（车机需开启「可被搜索」）");
}

void loop() {
  gWeb.loop();
  gDoor.loop(gBt);
  serviceBootLongPress();
  handleSerial();
  rfAutoTxService();
  gBt.loop();

  // NFC 刷卡：授权卡 → 手动开关门
  {
    String uid;
    if (gNfc.poll(uid)) {
      Serial.println("[NFC] card: " + uid);
      if (gNfc.isAuthorized(uid)) {
        Serial.println("[NFC] authorized -> toggle");
        gDoor.requestManualToggle(OpenSource::NFC);
      } else if (gNfc.authUid().length() == 0) {
        // 未注册任何卡：打印 UID 方便用户注册
        Serial.println("[NFC] 未注册卡，串口执行: nfcsave " + uid);
      } else {
        Serial.println("[NFC] 未授权卡");
      }
    }
  }

  // ===== 跟踪模式分发 =====
  int trackMode = gWeb.trackMode();  // 实时从 WebPortal 读取（网页可改）
  if (trackMode == TRACK_MODE_BLE) {
    // BLE：无→有=开（弱也开）；无→极强=库内唤醒不开；强→弱→无=关
    enum class BlePhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static BlePhase phase = BlePhase::WAIT_SIGNAL;

    // 手机连着 SoftAP 时停掉 BLE 周期扫描，否则 2.4G 抢射频 → 热点一会有一会无
    const bool wifiClient = WiFi.softAPgetStationNum() > 0;
    if (gBleScan.trackOn() && !wifiClient) {
      // runScan() 是阻塞的：busy 边沿在同一轮 trackPoll 内完成，不能靠 busy 跨轮判断
      const uint32_t prevScanEnd = gBleScan.lastScanEndMs();
      gBleScan.trackPoll(6000, 2000);
      const bool scanJustFinished = gBleScan.lastScanEndMs() != prevScanEnd;
      if (scanJustFinished) {
        int r = gBleScan.matchRssi();
        bool lost = gBleScan.lostCar();
        bool seen = gBleScan.lastMatchMs() != 0 &&
                    (millis() - gBleScan.lastMatchMs()) < BLE_SILENT_GAP_MS;
        bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
        bool isStrong = hasSignal && r >= RSSI_STRONG;
        // 首见就极强：人在库里蓝牙刚醒，不是“从远处走近”
        bool suddenVeryStrong = hasSignal && r >= RSSI_SUDDEN_STRONG;

        const char* phaseName =
            phase == BlePhase::WAIT_SIGNAL ? "WAIT" :
            phase == BlePhase::APPEARING  ? "APPEAR" :
            phase == BlePhase::STRONG     ? "STRONG" : "LEAVE";

        Serial.printf("[BLE] rssi=%d seen=%d strong=%d lost=%d phase=%s\n",
                      r, (int)hasSignal, (int)isStrong, (int)lost, phaseName);

        switch (phase) {
          case BlePhase::WAIT_SIGNAL:
            if (hasSignal && !suddenVeryStrong) {
              // 无→有：门口 -93 也要开，不等到 -80
              Serial.printf("[FSM] 无→有 rssi=%d，自动开\n", r);
              autoOpenThenArm("无→有");
              phase = BlePhase::APPEARING;
            } else if (suddenVeryStrong) {
              // 库内唤醒：不自动开；进 STRONG 但不 arm 关门，防丢失闪断连发 close
              phase = BlePhase::STRONG;
              Serial.printf("[FSM] 无→强跳变 rssi=%d，不自动开（库内唤醒？）\n", r);
            }
            break;

          case BlePhase::APPEARING:
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
            if (!hasSignal || lost) {
              gCloseArmed = true;
              autoCloseGuarded("强→弱→无");
              phase = BlePhase::WAIT_SIGNAL;
            } else if (isStrong) {
              phase = BlePhase::STRONG;
              Serial.println("[FSM] 弱→强（回到门口），取消离开");
            }
            break;
        }
      }
    } else if (wifiClient && gBleScan.busy() == false) {
      // 连着热点就不启新 BLE 扫；日志方便确认
      static uint32_t lastWifiSkipLog = 0;
      if (millis() - lastWifiSkipLog > 15000) {
        lastWifiSkipLog = millis();
        Serial.println("[BLE] track paused (SoftAP client connected)");
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

    if (gBt.autoTrack() && gBt.hasTarget()) {
      int r = gBt.lastRssi();
      bool seen = gBt.seenRecently(BLE_SILENT_GAP_MS);
      bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
      bool isStrong = hasSignal && r >= RSSI_STRONG;
      bool suddenVeryStrong = hasSignal && r >= RSSI_SUDDEN_STRONG;
      bool lost = !seen;

      bool edge = !clInited || (seen != clPrevSeen) ||
                  (seen && abs(r - clPrevRssi) >= 5);
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
          Serial.printf("[CLASSIC] rssi=%d seen=%d strong=%d lost=%d phase=%s\n",
                        r, (int)hasSignal, (int)isStrong, (int)lost, phaseName);
        }

        switch (clPhase) {
          case ClPhase::WAIT_SIGNAL:
            if (hasSignal && !suddenVeryStrong) {
              Serial.printf("[FSM] C 无→有 rssi=%d，自动开\n", r);
              autoOpenThenArm("经典无→有");
              clPhase = ClPhase::APPEARING;
            } else if (suddenVeryStrong) {
              clPhase = ClPhase::STRONG;
              Serial.printf("[FSM] C 无→强跳变 rssi=%d，不自动开（库内唤醒？）\n", r);
            }
            break;

          case ClPhase::APPEARING:
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
            if (lost) {
              gCloseArmed = true;
              autoCloseGuarded("经典强→弱→无");
              clPhase = ClPhase::WAIT_SIGNAL;
            } else if (isStrong) {
              clPhase = ClPhase::STRONG;
              Serial.println("[FSM] C 弱→强，取消离开");
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
                   " f=" + gBleScan.filter() + " | " + gNfc.debugLine());
  }
}
