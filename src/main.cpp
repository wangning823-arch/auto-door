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
        Serial.println("[CMD] " + gDoor.debugLine() + " | " + gBt.debugLine() +
                       " | mac=" + String(gMac) + " ap=" + gWeb.apSsid());
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
        gRf.capture();
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
            "cmds: status | open | close | rfcap | rflearn 0-3 | rfplay 0-3 | rfkeys | "
            "rfclear | nfcscan | nfcsave <uid> | wifi on|off | autotrack on|off");
      } else {
        Serial.println("[CMD] unknown, try help");
      }
      line = "";
    } else {
      if (line.length() < 64) line += c;
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
  gDoor.setRfEmit(rfEmitDoor);

  // 加载已学习的 RF 按键
  {
    bool any = false;
    for (int i = 0; i < RF_KEY_COUNT; i++) {
      String csv = gCfg.loadRfKey(i);
      if (csv.length() && gRf.setKeyFromCsv(i, csv.c_str())) any = true;
    }
    Serial.println("[BOOT] RF keys:");
    rfPrintKeys();
    if (!any) Serial.println("[BOOT] 尚未学习，串口: rflearn 0 再短按遥控");
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
    // BLE 模式：简化状态机 无→有→强=开；强→弱→无=关
    static bool bleWasBusy = false;
    enum class BlePhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static BlePhase phase = BlePhase::WAIT_SIGNAL;

    if (gBleScan.trackOn()) {
      bool before = gBleScan.busy();
      gBleScan.trackPoll(6000, 2000);
      if (!before && gBleScan.busy()) {
        gBt.setInquiryPaused(true);
        bleWasBusy = true;
      }
      if (bleWasBusy && !gBleScan.busy()) {
        gBt.setInquiryPaused(false);
        bleWasBusy = false;

        int r = gBleScan.matchRssi();
        bool lost = gBleScan.lostCar();
        bool seen = gBleScan.lastMatchMs() != 0 &&
                    (millis() - gBleScan.lastMatchMs()) < BLE_SILENT_GAP_MS;
        bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
        bool isStrong = hasSignal && r >= RSSI_STRONG;

        const char* phaseName =
            phase == BlePhase::WAIT_SIGNAL ? "WAIT" :
            phase == BlePhase::APPEARING  ? "APPEAR" :
            phase == BlePhase::STRONG     ? "STRONG" : "LEAVE";

        Serial.printf("[BLE] rssi=%d seen=%d strong=%d lost=%d phase=%s\n",
                      r, (int)hasSignal, (int)isStrong, (int)lost, phaseName);

        switch (phase) {
          case BlePhase::WAIT_SIGNAL:
            if (hasSignal && !isStrong) {
              phase = BlePhase::APPEARING;
              Serial.println("[FSM] 无→有（弱），等待变强...");
            } else if (hasSignal && isStrong) {
              Serial.println("[FSM] 无→强（跳变），不符合开门条件");
            }
            break;

          case BlePhase::APPEARING:
            if (isStrong) {
              gDoor.tryAutoOpen("无→有→强");
              phase = BlePhase::STRONG;
            } else if (!hasSignal || lost) {
              phase = BlePhase::WAIT_SIGNAL;
              Serial.println("[FSM] 弱信号消失，回 WAIT");
            }
            break;

          case BlePhase::STRONG:
            if (!isStrong && hasSignal) {
              phase = BlePhase::LEAVING;
              Serial.println("[FSM] 强→弱，车开始离开");
            } else if (!hasSignal || lost) {
              phase = BlePhase::WAIT_SIGNAL;
              Serial.println("[FSM] 强信号直接消失（异常），回 WAIT");
            }
            break;

          case BlePhase::LEAVING:
            if (!hasSignal || lost) {
              gDoor.tryAutoClose("强→弱→无");
              phase = BlePhase::WAIT_SIGNAL;
            } else if (isStrong) {
              phase = BlePhase::STRONG;
              Serial.println("[FSM] 弱→强（回到门口），取消离开");
            }
            break;
        }
      }
    }
  } else {
    // Classic 模式：渐变逻辑（用于小蚂蚁等无 BLE 车型）
    // 开门：渐近且 RSSI 足够强
    // 关门：渐离且信号消失一段时间
    static bool wasGradualOut = false;
    static uint32_t leftSinceMs = 0;

    if (gBt.autoTrack() && gBt.hasTarget()) {
      SignalTrend trend = gBt.trend();
      int rssi = gBt.lastRssi();

      // 开门：渐近 + 信号够强
      if (trend == SignalTrend::GRADUAL_IN && rssi >= RSSI_OPEN) {
        if (gDoor.tryAutoOpen("渐近")) {
          Serial.printf("[CLASSIC] AUTO OPEN rssi=%d\n", rssi);
        }
      }

      // 关门：渐离后信号消失
      if (trend == SignalTrend::GRADUAL_OUT) {
        if (!wasGradualOut) {
          wasGradualOut = true;
          leftSinceMs = millis();
          Serial.println("[CLASSIC] 渐离开始，等待信号消失...");
        }
        // 渐离且超过 T_CLEAR_MS 无信号 → 关
        if (!gBt.seenRecently(T_CLEAR_MS)) {
          if (gDoor.tryAutoClose("渐离+清空")) {
            Serial.println("[CLASSIC] AUTO CLOSE");
          }
          wasGradualOut = false;
        }
      } else {
        wasGradualOut = false;
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
