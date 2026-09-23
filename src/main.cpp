#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <ArduinoOTA.h>
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
#include "remote_cmd.h"

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
static bool gOtaBegun = false;
// OTA 写 flash 期间禁止碰 I2C/NFC（否则易把 PN532/总线拖死，升级后刷卡失效）
static volatile bool gOtaActive = false;
static volatile uint32_t gOtaActiveAtMs = 0;

static void otaDisarm(const char* why) {
  if (!gOtaActive) return;
  gOtaActive = false;
  Serial.printf("[OTA] disarm (%s)\n", why ? why : "?");
  if (gNfc.ok()) gNfc.setListen(true);
  else gNfc.kickRecover();
}

// STA 连上后启动 ArduinoOTA：传输期间暂停 Inquiry+NFC，结束后恢复
static void serviceOta() {
  if (!gWeb.staConnected()) {
    // STA 掉线可能打断 OTA：必须清 gOtaActive，否则刷卡路径被永久跳过
    otaDisarm("sta lost");
    if (gOtaBegun) {
      ArduinoOTA.end();
      gOtaBegun = false;
      gWeb.setOtaReady(false);
      Serial.println("[OTA] STA lost, OTA stopped");
    }
    return;
  }
  // 兜底：onStart 后若既无 onEnd/onError（网络半死），超时自动解除
  if (gOtaActive && (millis() - gOtaActiveAtMs) > 180000UL) {
    otaDisarm("timeout 180s");
  }
  if (!gOtaBegun) {
    ArduinoOTA.setHostname(gWeb.staHostname().c_str());
    ArduinoOTA.onStart([]() {
      Serial.println("[OTA] START " + String(gWeb.staHostname()) + ".local");
      gOtaActive = true;
      gOtaActiveAtMs = millis();
      gNfc.setListen(false);
      if (gBtStackInited) {
        gBt.setInquiryPaused(true);
        gBt.cancelActiveInquiry();
      }
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("[OTA] END (reboot)");
      gOtaActive = false;
      if (gBtStackInited) gBt.setInquiryPaused(false);
      if (gNfc.ok()) gNfc.setListen(true);
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
      static int lastPct = -1;
      int pct = t ? (int)(100u * p / t) : 0;
      if (pct != lastPct && (pct % 10 == 0 || pct == 100)) {
        lastPct = pct;
        Serial.printf("[OTA] %d%%\n", pct);
      }
    });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("[OTA] error %u\n", (unsigned)e);
      otaDisarm("error");
      if (gBtStackInited) gBt.setInquiryPaused(false);
    });
    ArduinoOTA.begin();
    gOtaBegun = true;
    gWeb.setOtaReady(true);
    Serial.println("[OTA] ready host=" + gWeb.staHostname() +
                   ".local ip=" + gWeb.staIp() + " fw=" FW_VERSION);
  }
  ArduinoOTA.handle();
}

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

// 远程令 → 与 TRIG 相同出口（MIAO 手动 toggle）
static void onRemoteCmd(const char* cmd) {
  if (!cmd) return;
  if (strcmp(cmd, "open") == 0) {
    // MVP：语音「打开车库」→ 与 TRIG 一致用 toggle，避免 doorState 误判卡死
    gDoor.requestManualToggle(OpenSource::MIAO);
    Serial.println("[REMOTE] open -> MIAO toggle");
  } else if (strcmp(cmd, "close") == 0) {
    gDoor.requestManualClose(OpenSource::MIAO);
    Serial.println("[REMOTE] close -> MIAO close");
  } else if (strcmp(cmd, "toggle") == 0) {
    gDoor.requestManualToggle(OpenSource::MIAO);
  }
}

static bool rfEmitDoor(bool open) {
  int idx = open ? RF_KEY_OPEN : RF_KEY_CLOSE;
  // 槽位被清/损坏时回退默认码再发，避免「码没了却只能打继电器」
  if (!gRf.keyValid(idx)) {
    const char* csv = open ? RF_DEFAULT_OPEN_CSV : RF_DEFAULT_CLOSE_CSV;
    if (gRf.setKeyFromCsv(idx, csv)) {
      if (Serial.availableForWrite() > 32)
        Serial.printf("[RF] key%d invalid → reload default\n", idx);
    }
  }
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

// 离场/信号消失：一律发关码，不看软件门态（门已关再关一次也无害）
static bool tryCloseIfOpen(const char* why) {
  gCloseArmed = true;
  return autoCloseGuarded(why);
}

// ===== 真无 + 离场 RSSI 趋势（开/关门共用）=====
// 开：仅「连续真无」之后再出现（含很弱）才开；短 miss 回来不算无→有
// 关：≥RSSI_TREND_MIN_N 个有效 RSSI 单调变弱且首末够弱；反弹否决；或长时间真无兜底
struct RssiTrendWin {
  int8_t buf[6];
  uint8_t n = 0;
  uint8_t head = 0;
  void clear() {
    n = 0;
    head = 0;
  }
  void push(int r) {
    if (r > 0 || r < -127) return;
    buf[head] = (int8_t)r;
    head = (uint8_t)((head + 1) % 6);
    if (n < 6) n++;
  }
  bool gradualLeave() const {
    if (n < RSSI_TREND_MIN_N) return false;
    int s[6];
    uint8_t start = (uint8_t)((head - n + 12) % 6);
    for (uint8_t i = 0; i < n; i++) s[i] = buf[(start + i) % 6];
    const uint8_t k = RSSI_TREND_MIN_N;
    const int* p = s + (n - k);
    for (uint8_t i = 0; i + 1 < k; i++) {
      // 只允许小幅上翘；像 -60,-80,-60 会在第二步被否决
      if (p[i + 1] > p[i] + RSSI_TREND_TOL_DB) return false;
    }
    if (p[0] - p[k - 1] < RSSI_TREND_DROP_DB) return false;
    return true;
  }
  void dump() const {
    Serial.print("[FSM] rssi trend:");
    for (uint8_t i = 0; i < n; i++) {
      uint8_t idx = (uint8_t)((head - n + i + 12) % 6);
      Serial.printf(" %d", (int)buf[idx]);
    }
    Serial.println();
  }
};

static RssiTrendWin gRssiTrend;
static bool gTrueNo = true;      // 上电视为「无」，首次有信号即可开
static bool gEverHadSignal = false;
static bool gLeaveQual = false;  // 离开趋势合格（可关）
static uint32_t gNoSigSince = 0; // 0=当前有信号

static void observeSignal(bool hasSignal, int rssi) {
  const uint32_t now = millis();
  if (hasSignal) {
    gNoSigSince = 0;
    gEverHadSignal = true;
    gRssiTrend.push(rssi);
    if (gRssiTrend.gradualLeave()) {
      if (!gLeaveQual) {
        Serial.printf("[FSM] 离场趋势合格 rssi=%d（≥%d 点单调变弱）\n", rssi,
                      (int)RSSI_TREND_MIN_N);
        gRssiTrend.dump();
      }
      gLeaveQual = true;
    }
    if (rssi >= RSSI_STRONG) {
      if (gLeaveQual) {
        gLeaveQual = false;
        gRssiTrend.clear();
        Serial.println("[FSM] 回到强信号 → 清除离场趋势");
      }
    }
    // 注意：不在这里清 gTrueNo，否则 observeSignal 后再判「真无→有」会永远为 false
  } else {
    if (gNoSigSince == 0) gNoSigSince = now;
    if (millisReached(now, gNoSigSince + RSSI_TRUE_SILENT_MS) && !gTrueNo) {
      gTrueNo = true;
      Serial.println("[FSM] 真无确认（连续无信号满，之后有信号才再开）");
    }
  }
}

// 是否该发关码：趋势合格后信号没了/变很远；或有史以来真无满离开静默
static bool shouldCloseBySignal(bool hasSignal, bool isFar) {
  if (gLeaveQual && (!hasSignal || isFar)) return true;
  if (gEverHadSignal && gTrueNo && gNoSigSince != 0 &&
      millisReached(millis(), gNoSigSince + RSSI_LEAVE_SILENT_MS)) {
    return true;
  }
  return false;
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
// 安全：短时联调用；超时自动 OFF。整夜 rfauto 会把 315/433 接收机堵死，
// 表现为原遥控/刷卡都开不了门，拔掉 ESP32 才恢复。
static bool gRfAutoTx = false;
static uint32_t gRfAutoNextMs = 0;
static uint32_t gRfAutoOnSinceMs = 0;
static const uint32_t RF_AUTO_INTERVAL_MS = 5000;

static void rfAutoTxForceOff(const char* why) {
  if (!gRfAutoTx && gCfg.loadRfAuto(false)) {
    gCfg.saveRfAuto(false);
  }
  gRfAutoTx = false;
  gRfAutoOnSinceMs = 0;
  Serial.printf("[RF] rfauto OFF (%s)\n", why ? why : "?");
}

static void rfAutoTxFire(const char* why) {
  if (!gRfAutoTx) return;
  if (!gRf.keyValid(RF_KEY_OPEN)) {
    Serial.println("[RF] AUTO TX 失败：key0 未学习");
    rfAutoTxForceOff("key0 invalid");
    return;
  }
  gRfAutoNextMs = millis() + RF_AUTO_INTERVAL_MS;
  Serial.printf("[RF] AUTO TX open (%s) GPIO%d...\n", why, PIN_RF_TX);
  gRf.playKey(RF_KEY_OPEN);
}

static void rfAutoTxService() {
  if (!gRfAutoTx) return;
  // 联调窗口超时：自动关并落盘，防止无人值守整夜发码
  if (gRfAutoOnSinceMs != 0 &&
      (millis() - gRfAutoOnSinceMs) >= RF_AUTO_MAX_MS) {
    rfAutoTxForceOff("session timeout");
    return;
  }
  if (!millisReached(millis(), gRfAutoNextMs)) return;
  rfAutoTxFire("interval");
}

// TX 卡死看门狗：非发射窗口内 DATA 仍为高 → 强制拉低（防载波堵死门机）
static void serviceRfTxSafety() {
  static uint32_t highSinceMs = 0;
  static uint32_t lastLogMs = 0;
  if (gRf.txBusy()) {
    highSinceMs = 0;
    return;
  }
  if (digitalRead(PIN_RF_TX) == HIGH) {
    if (highSinceMs == 0) {
      highSinceMs = millis();
      return;
    }
    if ((millis() - highSinceMs) >= RF_TX_STUCK_MS) {
      gRf.forceTxLow();
      uint32_t now = millis();
      if (now - lastLogMs > 2000) {
        lastLogMs = now;
        Serial.printf("[RF][SAFE] TX=GPIO%d 空闲期持续高电平 → 已拉低（防堵门机）\n",
                      PIN_RF_TX);
      }
      highSinceMs = 0;
    }
  } else {
    highSinceMs = 0;
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
      gWeb.startStaFromStore();
      Serial.println("[BOOT] force SoftAP ON -> " + gWeb.apSsid() +
                     " pass=" + AP_PASSWORD);
      Serial.println("[BOOT] 手机连热点后打开 http://192.168.4.1/");
      Serial.println("[BOOT] STA ip=" + gWeb.staIp() + " ota=" +
                     gWeb.staHostname() + ".local");
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
          Serial.println("[CMD] ap_ip=" + WiFi.softAPIP().toString());
        }
        Serial.println("[CMD] sta=" + String(gWeb.staConnected() ? "up" : "down") +
                       " ip=" + gWeb.staIp() + " host=" + gWeb.staHostname() +
                       ".local ota=" + String(gOtaBegun ? "on" : "off"));
      } else if (line == "wifi off") {
        gCfg.saveWifiEnabled(false);
        gWeb.stopAp();   // 内部已 kickRecover
        gWeb.stopSta();
        gBt.setInquiryPaused(false);
        Serial.println("[CMD] WiFi AP+STA OFF + saved (BT inquiry free)");
        // 关热点后若尚未 init BT，立刻起栈，便于测自动门
        if (!gBtStackInited) initBtStacks();
      } else if (line == "wifi on") {
        gCfg.saveWifiEnabled(true);
        if (gWeb.startAp()) {
          gWeb.startStaFromStore();
          Serial.println("[CMD] WiFi ON " + gWeb.apSsid() + " " +
                         WiFi.softAPIP().toString() + " sta=" + gWeb.staIp());
        } else {
          Serial.println("[CMD] WiFi ON failed");
        }
      } else if (line == "wifi restart") {
        Serial.println("[CMD] WiFi restart SoftAP+HTTP...");
        gWeb.stopAp();
        delay(200);
        bool ok2 = gWeb.startAp();
        if (ok2) gWeb.startStaFromStore();
        Serial.printf("[CMD] wifi restart -> %s ip=%s sta=%d\n",
                      ok2 ? "OK" : "FAIL",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
      } else if (line == "remote on") {
        remoteCmdSetEnabled(true);
      } else if (line == "remote off") {
        remoteCmdSetEnabled(false);
      } else if (line == "wifi status") {
        Serial.printf("[CMD] mode=%d ap=%s ip=%s sta=%d apmac=%s heap=%u bt=%d\n",
                      (int)WiFi.getMode(), gWeb.apSsid().c_str(),
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum(),
                      WiFi.softAPmacAddress().c_str(),
                      (unsigned)ESP.getFreeHeap(), (int)gBtStackInited);
        Serial.println("[CMD] sta=" + String(gWeb.staConnected() ? "up" : "down") +
                       " ip=" + gWeb.staIp() + " host=" + gWeb.staHostname() +
                       ".local ota=" + String(gOtaBegun ? "on" : "off") +
                       " rssi=" + String(gWeb.staConnected() ? WiFi.RSSI() : 0));
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
          rfAutoTxForceOff("serial");
          Serial.println("[RF] rfauto OFF（已保存）");
        } else {
          gRfAutoTx = true;
          gRfAutoNextMs = millis();
          gRfAutoOnSinceMs = millis();
          gCfg.saveRfAuto(true);
          Serial.printf(
              "[RF] rfauto ON：每 %ums 发 key0，最长 %ums 后自动 OFF（防堵门机）\n",
              (unsigned)RF_AUTO_INTERVAL_MS, (unsigned)RF_AUTO_MAX_MS);
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
            "cmds: status | open | close | rfcap | rfstop | rflearn 0-3 | rfplay 0-3 | rfauto on|off (max 2min) | rfloop 0 | rfbench 0 6 | rfcloop | rfcarrier | rfkeys | "
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
  // 尽早钳位 RF TX，避免上电到 gRf.begin 之间脚位浮空乱发
  pinMode(PIN_RF_TX, OUTPUT);
  digitalWrite(PIN_RF_TX, LOW);
  remoteCmdSetHandler(onRemoteCmd);
  gCfg.begin();
  remoteCmdBegin(&gCfg);
  if (gCfg.loadRemote(false)) {
    remoteCmdSetEnabled(true);
  } else {
    Serial.println("[REMOTE] NVS off — 串口 remote on 开启并保存");
  }
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

  // 上电：默认不恢复 rfauto。旧工具会自动 rfauto on 并写 NVS，
  // 整夜每 5s 发码会堵死门机接收（原遥控/刷卡全失效，拔电才恢复）。
  gRfAutoTx = false;
  gRfAutoOnSinceMs = 0;
  if (gCfg.loadRfAuto(false)) {
    gCfg.saveRfAuto(false);
    Serial.println(
        "[BOOT][WARN] 发现 NVS rfauto=ON → 已强制 OFF 并清除"
        "（避免整夜发射干扰门机；联调请手动 rfauto on，2 分钟自动关）");
  } else {
    Serial.println("[BOOT] rfauto=OFF，串口: rfauto on 可开启（限时）");
  }
  gRf.forceTxLow();

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

  Serial.println("[BOOT] ready. fw=" FW_VERSION " wifi=" +
                 String(wifiOn ? 1 : 0) +
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
    if (gWeb.staConfigured()) {
      Serial.println("[BOOT] 已配家庭 Wi‑Fi，将连 STA：主机 " + gWeb.staHostname() +
                     ".local（OTA）");
    }
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
  serviceOta();
  gDoor.loop(gBt);
  serviceBootLongPress();
  handleSerial();
  rfAutoTxService();
  serviceRfTxSafety();
  // 先跑蓝牙调度，再跑 NFC：避免 I2C 抢在 Inquiry/BLE 之前占满 loop
  serviceBtStackInit();
  if (gBtStackInited) {
    gBt.loop();
    gBleBond.service();
  }

  // 远程令：蓝牙忙（Inquiry/BLE 扫描）绝不发 HTTP；STA 已连才轮询
  {
    const bool btBusy =
        gBtStackInited && (gBt.inquiryBusy() || gBleScan.busy());
    remoteCmdService(btBusy, gWeb.staConnected());
  }

  // ===== NFC 与 WiFi/蓝牙的共存策略 =====
  // 射频层：NFC=13.56MHz，BT=2.4GHz，互不干扰。
  // 软件层：PN532 poll/hwInit 会阻塞 loop，会拖慢网页和 Inquiry → 按场景调度。
  //  · SoftAP 有客户端：只拉长 NFC 间隔，不整段停（否则手机连网页时刷卡失效）
  //  · OTA 写 flash：完全不碰 NFC/I2C
  //  · 无网/热点无人：NFC + 蓝牙必须同时工作；NFC 降频，且不在 Inquiry 忙时做 hwInit
  //  · 上电 hwInit 优先：即使热点有人也要把 NFC 起起来（配 Wi‑Fi 时才能用）
  {
    const bool apOn = gWeb.apActive();
    const bool apClient = apOn && WiFi.softAPgetStationNum() > 0;
    // 未就绪时必须能跑 poll/maybeRecover，即使热点有人（否则上电 init 永不发生）
    const bool nfcNeedInit = !gNfc.ok();
    const bool btTrack =
        gBtStackInited && (gBt.autoTrack() || gBleScan.trackOn());
    const bool btBusy = gBtStackInited && gBt.inquiryBusy();
    const bool bleBusy = gBtStackInited && gBleScan.busy();

    // OTA 写 flash：完全不碰 NFC/I2C
    if (gOtaActive) {
      // fall through — 不 poll、不 init
    } else {
      static bool nfcWasQuiet = false;
      // 热点有人：不再整段停 NFC（升级 OTA 后手机常连网页 → 刷卡会“死”）。
      // 只拉长间隔让出 loop；真正要停的是 OTA 传输期。
      if (apClient != nfcWasQuiet) {
        nfcWasQuiet = apClient;
        if (!apClient && !gNfc.ok()) {
          gNfc.kickRecover();  // 客户端断开后立刻补一次 init
        }
      }

      // 跟踪中加大间隔；热点有人时再慢一点，给网页留带宽
      if (btTrack) {
        gNfc.setPollGapMs(1200);
      } else if (apClient) {
        gNfc.setPollGapMs(800);
      } else if (apOn) {
        gNfc.setPollGapMs(500);
      } else {
        gNfc.setPollGapMs(350);
      }

      // 已就绪但 listen 被关掉（OTA 失败/手动）→ 恢复
      if (gNfc.ok() && !gNfc.listen() && !gOtaActive) {
        gNfc.setListen(true);
      }

      // 未就绪：即使热点有人 / Inquiry 忙也允许 init
      if (nfcNeedInit) {
        String uid0;
        gNfc.poll(uid0);  // 内部 maybeRecover / 上电自动 init
      } else {
        String uid;
        if (gNfc.poll(uid)) {
          const bool auth = gNfc.isAuthorized(uid);
          // 先开/关门，再打日志：串口 TX 满时 println 会阻塞，不能挡 RF
          if (auth) {
            gDoor.requestManualToggle(OpenSource::NFC);
            Serial.println("[NFC] card: " + uid + " authorized → RF");
          } else if (gNfc.authUid().length() == 0) {
            Serial.println("[NFC] card: " + uid + " 未注册卡，串口: nfcsave " + uid);
          } else {
            Serial.println("[NFC] card: " + uid + " 未授权卡");
          }
        }
      }
    }
  }

  // ===== 跟踪模式分发 =====
  int trackMode = gWeb.trackMode();
  if (trackMode == TRACK_MODE_BLE) {
    enum class BlePhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static BlePhase phase = BlePhase::WAIT_SIGNAL;

    const bool wifiClient = WiFi.softAPgetStationNum() > 0;
    const bool apYield = gWeb.wifiRfPriority() || gWeb.rfQuietActive();
    if (gBtStackInited && gBleScan.trackOn() && !wifiClient) {
      const uint32_t prevScanEnd = gBleScan.lastScanEndMs();
      if (!apYield) {
        gBleScan.trackPoll(BLE_TRACK_INTERVAL_MS, BLE_TRACK_SCAN_MS);
      }
      const bool scanJustFinished = gBleScan.lastScanEndMs() != prevScanEnd;
      int r = gBleScan.matchRssi();
      bool lost = gBleScan.lostCar();
      bool seen = gBleScan.lastMatchMs() != 0 &&
                  (millis() - gBleScan.lastMatchMs()) < BLE_SILENT_GAP_MS;
      bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
      bool isStrong = hasSignal && r >= RSSI_STRONG;
      bool isFar = hasSignal && r <= RSSI_FAR_CLOSE;

      if (scanJustFinished) observeSignal(hasSignal, hasSignal ? r : 0);

      if (scanJustFinished) {
        Serial.printf(
            "[BLE] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d phase=%d\n",
            r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
            (int)gLeaveQual, (int)phase);

        switch (phase) {
          case BlePhase::WAIT_SIGNAL:
            if (hasSignal && gTrueNo) {
              Serial.printf("[FSM] 真无→有 rssi=%d，发开码\n", r);
              gTrueNo = false;
              autoOpenThenArm("真无→有");
              phase = BlePhase::APPEARING;
              break;
            }
            if (shouldCloseBySignal(hasSignal, isFar)) {
              Serial.println("[FSM] WAIT 离场条件 → 发关码");
              tryCloseIfOpen("WAIT离场");
              break;
            }
            break;

          case BlePhase::APPEARING:
            if (isStrong) {
              autoOpenThenArm("有→强");
              phase = BlePhase::STRONG;
              break;
            }
            if (shouldCloseBySignal(hasSignal, isFar)) {
              Serial.println("[FSM] APPEAR 离场条件 → 发关码");
              tryCloseIfOpen("APPEAR离场");
              phase = BlePhase::WAIT_SIGNAL;
              break;
            }
            if (!hasSignal) phase = BlePhase::WAIT_SIGNAL;
            break;

          case BlePhase::STRONG:
            if (shouldCloseBySignal(hasSignal, isFar)) {
              Serial.println("[FSM] STRONG 离场条件 → 发关码");
              tryCloseIfOpen("STRONG离场");
              phase = BlePhase::WAIT_SIGNAL;
              break;
            }
            if (!isStrong && hasSignal) {
              phase = BlePhase::LEAVING;
            } else if (!hasSignal) {
              phase = BlePhase::WAIT_SIGNAL;
            }
            break;

          case BlePhase::LEAVING:
            if (isStrong) {
              phase = BlePhase::STRONG;
              gLeaveQual = false;
              gRssiTrend.clear();
              Serial.println("[FSM] 弱→强，取消离开");
              break;
            }
            if (shouldCloseBySignal(hasSignal, isFar)) {
              Serial.println("[FSM] LEAVING 离场条件 → 发关码");
              tryCloseIfOpen("LEAVING离场");
              phase = BlePhase::WAIT_SIGNAL;
              break;
            }
            if (hasSignal) phase = BlePhase::STRONG;
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
    enum class ClPhase : uint8_t {
      WAIT_SIGNAL, APPEARING, STRONG, LEAVING,
    };
    static ClPhase clPhase = ClPhase::WAIT_SIGNAL;
    static uint32_t lastClLogMs = 0;
    static bool clPrevSeen = false;
    static int clPrevRssi = -999;
    static bool clInited = false;

    if (gBtStackInited && gBt.autoTrack() && gBt.hasTarget()) {
      int r = gBt.lastRssi();
      bool seen = gBt.seenRecently(BLE_SILENT_GAP_MS);
      bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
      bool isStrong = hasSignal && r >= RSSI_STRONG;
      bool isFar = hasSignal && r <= RSSI_FAR_CLOSE;
      bool lost = !seen;

      if (!clInited || seen != clPrevSeen || (seen && abs(r - clPrevRssi) >= 3) ||
          (!seen && clPrevSeen)) {
        observeSignal(hasSignal, hasSignal ? r : 0);
      }
      if (!seen) observeSignal(false, 0);

      const bool closeDue = shouldCloseBySignal(hasSignal, isFar);

      clInited = true;
      clPrevSeen = seen;
      clPrevRssi = r;

      if (millis() - lastClLogMs >= 2000) {
        lastClLogMs = millis();
        Serial.printf(
            "[CLASSIC] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d phase=%d\n",
            r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
            (int)gLeaveQual, (int)clPhase);
      }

      switch (clPhase) {
        case ClPhase::WAIT_SIGNAL:
          if (hasSignal && gTrueNo) {
            Serial.printf("[FSM] C 真无→有 rssi=%d，发开码\n", r);
            gTrueNo = false;
            autoOpenThenArm("经典真无→有");
            clPhase = ClPhase::APPEARING;
            break;
          }
          if (closeDue) {
            Serial.println("[FSM] C WAIT 离场条件 → 发关码");
            tryCloseIfOpen("经典WAIT离场");
          }
          break;

        case ClPhase::APPEARING:
          if (isStrong) {
            autoOpenThenArm("经典有→强");
            clPhase = ClPhase::STRONG;
            break;
          }
          if (closeDue) {
            Serial.println("[FSM] C APPEAR 离场条件 → 发关码");
            tryCloseIfOpen("经典APPEAR离场");
            clPhase = ClPhase::WAIT_SIGNAL;
            break;
          }
          if (lost) clPhase = ClPhase::WAIT_SIGNAL;
          break;

        case ClPhase::STRONG:
          if (closeDue) {
            Serial.println("[FSM] C STRONG 离场条件 → 发关码");
            tryCloseIfOpen("经典STRONG离场");
            clPhase = ClPhase::WAIT_SIGNAL;
            break;
          }
          if (!isStrong && hasSignal) {
            clPhase = ClPhase::LEAVING;
          } else if (lost) {
            clPhase = ClPhase::WAIT_SIGNAL;
          }
          break;

        case ClPhase::LEAVING:
          if (isStrong) {
            clPhase = ClPhase::STRONG;
            gLeaveQual = false;
            gRssiTrend.clear();
            Serial.println("[FSM] C 弱→强，取消离开");
            break;
          }
          if (closeDue) {
            Serial.println("[FSM] C LEAVING 离场条件 → 发关码");
            tryCloseIfOpen("经典LEAVING离场");
            clPhase = ClPhase::WAIT_SIGNAL;
            break;
          }
          if (hasSignal) clPhase = ClPhase::STRONG;
          break;
      }
    }
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 8000) {
    lastLog = millis();
    // 串口缓冲不空闲就跳过周期日志，避免 TX 满时 println 拖死 loop
    if (Serial.availableForWrite() > 128) {
      Serial.println("[LOG] " + gDoor.debugLine() + " | " + gBt.debugLine() +
                     " | ble_rssi=" + String(gBleScan.matchRssi()) +
                     " bond=" + String(gBleBond.hasIrk() ? "Y" : "N") + " | " +
                     gNfc.debugLine());
    }
  }
}
