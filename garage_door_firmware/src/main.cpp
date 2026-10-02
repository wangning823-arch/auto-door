#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "classic_tracker.h"
#include "door_fsm.h"
#include "config_store.h"
#include "web_portal.h"
#include "ble_scan.h"
#include "rf_capture.h"
#include "nfc_reader.h"
#include "default_rf_keys.h"
#include "ble_bond.h"
#include "remote_cmd.h"
#include "log_ship.h"
#include "crash_snap.h"
#include "remote_ota.h"
#include "status_report.h"
#include "http_client.h"
#include "device_id.h"

// ===== 车库门智能控制器 P0.1 =====
// SoftAP 网页配置车机 MAC + F0/F1a/F2a/F3
// 手机连热点 GarageDoor-xxxx / 12345678 → 浏览器打开 192.168.4.1

static ClassicTracker gBt;
static DoorFsm gDoor;
static ConfigStore gCfg;
static WebPortal gWeb;
BleScanTool gBleScan;
static RfCapture gRf;
static NfcReader gNfc;
static char gMac[24] = CAR_BT_MAC;
static bool gBtStackInited = false;
// OTA 写 flash 期间禁止碰 I2C/NFC（否则易把 PN532/总线拖死，升级后刷卡失效）
// 由 VPS 远程 OTA 的 busy hook 置位（ArduinoOTA/桌面 espota 已移除：
// 其 UDP parsePacket 每 loop 试分配 1460B、~200失败/秒，是 DEF 池头号搅动者）
static volatile bool gOtaActive = false;

// http worker 发送前查询：蓝牙正在占射频（Inquiry/BLE 扫描）就等空隙
static bool httpBtRadioBusy() {
  if (!gBtStackInited) return false;
  // btQuietForHttp 含 inquiry + postQuiet 保护窗(800ms)：worker 不在保护窗里
  // 起发——否则请求跨进下一发 inquiry（4s 时代跨窗打架、堆抖动的病根）。
  // ACTIVE 整窗内 btQuietForHttp=false → 照常放行。
  return gBt.inquiryBusy() || gBt.btQuietForHttp() || gBleScan.busy();
}

// 经典 BT + BLE 配对栈：SoftAP 调试时推迟，优先让网页先出来
// 二选一：经典只起 SerialBT；BLE 只起 BLEDevice —— 绝不双栈同开
static void initBtStacks() {
  if (gBtStackInited) return;
  const bool classic = (gWeb.trackMode() == TRACK_MODE_CLASSIC);
  Serial.printf("[BT] init start t=%ums mode=%s mac=%s\n", (unsigned)millis(),
                classic ? "CLASSIC" : "BLE", gMac);

  if (classic) {
    bool classicOk = gBt.begin(gMac);
    if (!classicOk) {
      Serial.println("[BOOT] Classic BT init failed");
    } else {
      Serial.println("[BT] classic OK (BLE stack not started)");
    }
    bool autoOn = gCfg.loadAutoTrack(true);
    if (gBt.hasTarget()) autoOn = true;
    gBt.setAutoTrack(autoOn);
    gBleScan.setTrack(false);  // 经典模式强制关 BLE 跟踪
    Serial.println("[BOOT] trackMode=CLASSIC autotrack=" +
                   String(autoOn ? "ON" : "OFF") + " ble_track=OFF");
    // 不 gBleBond.begin()：SerialBT+BLE 双栈 heap≈29KB → SSL/网页挂
    Serial.println("[BT] skip BLE bond (classic track) — save heap for STA/HTTPS");
  } else {
    // BLE 模式：不 gBt.begin()，不跑经典 Inquiry
    gBt.setAutoTrack(false);
    Serial.println("[BT] BLE mode — skip classic SerialBT");
    Serial.println("[BOOT] BLE bond/IRK init...");
    gBleBond.begin();
    Serial.println("[BT] bond init done begun=" +
                   String(gBleBond.begun() ? 1 : 0));
    if (gBleBond.hasIrk()) {
      gBleScan.setTrack(true);
      Serial.println("[BOOT] IRK track ON (paired phone)");
    } else {
      gBleScan.setTrack(false);
      Serial.println("[BOOT] no IRK yet — BLE track OFF until pair");
    }
  }
  gBtStackInited = true;
  logShipf("[BT] init stacks COMPLETE heap=%u begun=%d classic_ready=%d",
           (unsigned)ESP.getFreeHeap(), (int)gBleBond.begun(),
           (int)gBt.ready());
}

static void serviceBtStackInit() {
  if (gBtStackInited) return;

  static bool sawApOn = false;
  static uint32_t apOffAtMs = 0;

  // SoftAP 开着：记下来，绝不起栈（避免与关热点/网页抢时序复位）
  if (gWeb.apActive()) {
    sawApOn = true;
    apOffAtMs = 0;
    return;
  }

  // 无热点：等 STA 稳定（或超时）再起 Bluedroid
  // 开机与 WiFi.begin 同时 SerialBT.begin → SW_CPU_RESET 死循环
  if (!sawApOn) {
    if (millis() < 8000) return;
    if (gWeb.staConfigured() && !gWeb.staConnected() && millis() < 20000) {
      return;
    }
    Serial.printf("[BT] deferred init t=%ums sta=%d\n", (unsigned)millis(),
                  (int)gWeb.staConnected());
    initBtStacks();
    return;
  }

  // 从有热点 → 关掉：等 3s，让 WiFi 彻底稳再 Bluedroid
  if (apOffAtMs == 0) {
    apOffAtMs = millis();
    Serial.println("[BT] SoftAP off → 等 3000ms 再 init stacks");
    return;
  }
  if ((millis() - apOffAtMs) < 3000) return;
  initBtStacks();
}

static bool rfSaveKeyCb(int idx, const char* csv) {
  return gCfg.saveRfKey(idx, csv);
}

// 远程令：按语义开/关，禁止把 open 做成 toggle
// （否则门已开时再喊「打开车库」会按上次 OPEN 翻成关）
static void onRemoteCmd(const char* raw) {
  if (!raw || !*raw) return;
  // ===== 无参指令 =====
  if (strcmp(raw, "open") == 0) {
    gDoor.requestManualOpen(OpenSource::MIAO);
    Serial.println("[REMOTE] open -> MIAO open");
    return;
  }
  if (strcmp(raw, "close") == 0) {
    gDoor.requestManualClose(OpenSource::MIAO);
    Serial.println("[REMOTE] close -> MIAO close");
    return;
  }
  if (strcmp(raw, "toggle") == 0) {
    // 仅显式 toggle（如单按钮场景）才翻转
    gDoor.requestManualToggle(OpenSource::MIAO);
    Serial.println("[REMOTE] toggle -> MIAO toggle");
    return;
  }
  if (strcmp(raw, "update") == 0) {
    // 服务端发现新固件 / 手动触发 → 立刻查 OTA
    logShipf("[REMOTE] update cmd -> ota check");
    remoteOtaCheckNow();
    return;
  }
  if (strcmp(raw, "reboot") == 0) {
    // 远程软复位。两个用途：
    //  1) 验证 RTC noinit 真的跨复位保留——重启后必须出现
    //     [LOGSHIP] resume N bytes from prev run，否则说明方案没生效
    //  2) 不带 flush：日志环原样留在 RTC，重启后由 logShipBegin 续传
    if (remoteOtaActive()) {
      logShipf("[REMOTE] reboot refused: ota active");
      return;
    }
    logShipf("[REMOTE] reboot -> soft reset (ring kept in RTC)");
    gNfc.stopForOta();  // 复位前清 NFC 总线，防 PN532 半截事务卡死
    delay(200);  // 让串口把这行打完，便于现场对照
    ESP.restart();
    return;
  }

  // ===== 带参指令："<verb> <arg...>"（VPS 控制台替代本地网页的配置通道）=====
  char verb[20];
  const char* sp = strchr(raw, ' ');
  const char* arg = "";
  if (!sp) {
    // 服务端会 strip 尾部空格：pairpin 无参 = 清除 PIN（与网页「留空清除」一致）
    if (strcmp(raw, "pairpin") == 0) {
      gBleBond.setPairingPin(String(""));
      logShipf("[REMOTE] pairpin cleared");
      return;
    }
    logShipf("[REMOTE] unknown cmd: %s", raw);
    return;
  }
  size_t vl = (size_t)(sp - raw);
  if (vl == 0 || vl >= sizeof(verb)) {
    logShipf("[REMOTE] bad cmd len=%u", (unsigned)vl);
    return;
  }
  memcpy(verb, raw, vl);
  verb[vl] = '\0';
  arg = sp + 1;

  if (strcmp(verb, "mac") == 0) {
    String m(arg);
    m.trim();
    m.toUpperCase();
    if (m.length() != 17) {
      logShipf("[REMOTE] mac reject len=%u", (unsigned)m.length());
      return;
    }
    m.toCharArray(gMac, sizeof(gMac));
    gCfg.saveMac(m);
    if (gWeb.trackMode() == TRACK_MODE_CLASSIC) gBt.begin(gMac);
    logShipf("[REMOTE] mac set+saved %s", gMac);
  } else if (strcmp(verb, "mode") == 0) {
    int m = atoi(arg);
    if (m == TRACK_MODE_BLE || m == TRACK_MODE_CLASSIC) {
      gWeb.setTrackMode(m);  // 内部存 NVS，变更后约 1.2s 自重启独占栈
      logShipf("[REMOTE] mode -> %d", m);
    } else {
      logShipf("[REMOTE] mode reject: %s", arg);
    }
  } else if (strcmp(verb, "pair") == 0) {
    if (gWeb.trackMode() != TRACK_MODE_BLE) {
      logShipf("[REMOTE] pair ignored (classic mode)");
    } else if (strncmp(arg, "off", 3) == 0) {
      gBleBond.closePairingWindow("remote");
      logShipf("[REMOTE] pair window closed");
    } else {
      int sec = 90;
      if (strncmp(arg, "on ", 3) == 0 && arg[3] != '\0') sec = atoi(arg + 3);
      if (sec <= 0) sec = 90;
      gBleBond.requestOpenPairing((uint32_t)sec * 1000);
      logShipf("[REMOTE] pair window %ds", sec);
    }
  } else if (strcmp(verb, "pairpin") == 0) {
    gBleBond.setPairingPin(String(arg));  // 空参数 = 清除
    logShipf("[REMOTE] pairpin %s", arg[0] ? "set" : "cleared");
  } else if (strcmp(verb, "autotrack") == 0) {
    if (strncmp(arg, "on", 2) == 0) {
      if (gWeb.trackMode() != TRACK_MODE_CLASSIC) {
        logShipf("[REMOTE] autotrack ignored (BLE mode)");
      } else {
        gBt.setAutoTrack(true);
        gCfg.saveAutoTrack(true);
        logShipf("[REMOTE] autotrack ON");
      }
    } else if (strncmp(arg, "off", 3) == 0) {
      gBt.setAutoTrack(false);
      gCfg.saveAutoTrack(false);
      logShipf("[REMOTE] autotrack OFF");
    }
  } else if (strcmp(verb, "nfcinit") == 0) {
    bool ok = gNfc.forceInit();
    logShipf("[REMOTE] nfcinit %s", ok ? "OK" : "FAIL");
  } else if (strcmp(verb, "wifista") == 0) {
    // "ssid pass"：第一个空格切分，密码可含空格；只有 ssid = 保留旧密码
    String a(arg);
    a.trim();
    int spIdx = a.indexOf(' ');
    String ssid = spIdx > 0 ? a.substring(0, spIdx) : a;
    String pass = spIdx > 0 ? a.substring(spIdx + 1) : String();
    ssid.trim();
    pass.trim();
    if (ssid.length() == 0) {
      logShipf("[REMOTE] wifista reject: empty ssid");
      return;
    }
    if (pass.length() == 0 && gCfg.loadStaSsid() == ssid) {
      pass = gCfg.loadStaPass();  // 与网页一致：同 SSID 留空 = 不改密码
    }
    bool ok = gCfg.saveSta(ssid, pass);
    gWeb.startStaFromStore();
    logShipf("[REMOTE] wifista ssid=%s ok=%d", ssid.c_str(), ok ? 1 : 0);
  } else if (strcmp(verb, "web") == 0) {
    // 本地网页软下线开关（迁移 VPS 控制台后默认关；出问题串口/远程都可救回）
    if (strncmp(arg, "off", 3) == 0) {
      gCfg.saveWebUi(false);
      gWeb.setUiEnabled(false);
      logShipf("[REMOTE] local web OFF (STA/remote/logs unaffected)");
    } else if (strncmp(arg, "on", 2) == 0) {
      gCfg.saveWebUi(true);
      gWeb.setUiEnabled(true);
      logShipf("[REMOTE] local web ON");
    }
  } else {
    logShipf("[REMOTE] unknown cmd: %s", verb);
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
// 开门后是否再次见过强信号（门外安装：门口出现过强）
static bool gStrongAfterOpen = false;
static uint32_t gLastAutoCloseMs = 0;
static bool gLeaveQual = false;
static bool gTrueNo = true;  // 上电视为「无」，首次有信号即可开

// ===== 门外安装（ESP32 在车库门外）=====
// 上电保守：先当「库内未知」，弱路径不开；见门口持续强才放开
static bool gInGarage = true;
static bool gHadDoorStrong = false;       // 曾在门口见过强信号
static uint32_t gLastStrongMs = 0;        // 最近一次 ≥RSSI_STRONG
static uint32_t gLostSinceStrongMs = 0;   // 强后信号丢失起始（0=当前未丢/从未强）
static uint8_t gStrongStreak = 0;         // 滑动窗口内强样本计数
static uint32_t gStrongStreakSince = 0;   // 本窗口起始
static uint32_t gFirstStrongInWinMs = 0;  // 窗口内首次强（校验 HOLD）
static bool gStrongOpenReady = false;     // 窗口内强次数达标，允许自动开

// 误触取证：最近 8 次信号观测（-127=无），离场资格/发关码时上送 VPS
static int8_t gSigLog[8];
static uint8_t gSigLogN = 0, gSigLogHead = 0;
static int gLastSigRssi = -127;

static void sigLogPush(int rssi) {
  int v = rssi;
  if (v > 127) v = 127;
  if (v < -127) v = -127;
  gSigLog[gSigLogHead] = (int8_t)v;
  gSigLogHead = (uint8_t)((gSigLogHead + 1) % 8);
  if (gSigLogN < 8) gSigLogN++;
}

static void sigLogShip(const char* tag, int rssi) {
  char body[80];
  size_t off = 0;
  for (uint8_t i = 0; i < gSigLogN && off < sizeof(body) - 8; i++) {
    uint8_t idx = (uint8_t)((gSigLogHead - gSigLogN + i + 16) % 8);
    off += snprintf(body + off, sizeof(body) - off, "%s%d", i ? " " : "",
                    (int)gSigLog[idx]);
  }
  logShipf("[FSM] sig %s rssi=%d leaveQ=%d strongAfter=%d trueNo=%d inGar=%d | %s",
           tag, rssi, (int)gLeaveQual, (int)gStrongAfterOpen, (int)gTrueNo,
           (int)gInGarage, body);
}

static void clearLeaveQual(const char* why) {
  if (gLeaveQual) {
    gLeaveQual = false;
    if (why) Serial.printf("[FSM] 清除离场资格 (%s)\n", why);
  }
}

// 弱路径开门：库内 / 手动关抑制期内一律拒绝（熄火后蓝牙仍亮会抖 无→有/弱→强）
static bool openWeakPathAllowed() {
  if (gInGarage) return false;
  if (gDoor.autoOpenSuppressActive()) return false;
  return true;
}

// 门口持续强开门：允许出库到门口；须 gStrongOpenReady
static bool openStrongPathAllowed() { return gStrongOpenReady; }

static bool autoOpenThenArm(const char* why, bool allowDuringSuppress = false) {
  bool ok = gDoor.tryAutoOpen(why, allowDuringSuppress);
  if (ok) {
    gCloseArmed = true;
    gStrongAfterOpen = false;  // 开门后须再确认门口强/离场
    clearLeaveQual("开门重置");
    // 开门成功 = 真在用门（出库/回场贴近）→ 解除库内弱开锁
    gInGarage = false;
    sigLogShip("autoOpen", gLastSigRssi);
  }
  return ok;
}

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

// 离场/信号消失：发关码。成功后清离场资格 + 限频，并屏蔽弱路径开
static bool tryCloseIfOpen(const char* why) {
  gCloseArmed = true;
  const uint32_t now = millis();
  if (gLastAutoCloseMs != 0 &&
      (now - gLastAutoCloseMs) < AUTO_CLOSE_MIN_INTERVAL_MS) {
    return false;
  }
  bool ok = autoCloseGuarded(why);
  sigLogShip(ok ? "autoClose" : "closeSkip", gLastSigRssi);
  if (ok) {
    gLastAutoCloseMs = now;
    clearLeaveQual("已发关码");
    gStrongAfterOpen = false;
    // 关完门：车多半在库内或已走远，禁止库内漏扫弱路径再顶开
    gInGarage = true;
    gDoor.setAutoOpenSuppress(AUTO_CLOSE_SUPPRESS_MS);
    Serial.printf("[FSM] 自动关已发 (%s)，离场资格已清 inGarage=1\n",
                  why ? why : "?");
  }
  return ok;
}

// ===== 真无 + 离场 RSSI 趋势（开/关门共用）=====
struct RssiTrendWin {
  int8_t buf[6];
  uint32_t ts[6];  // 采样时刻（时间窗淘汰用）
  uint8_t n = 0;
  uint8_t head = 0;
  void clear() {
    n = 0;
    head = 0;
  }
  void push(int r, uint32_t now) {
    if (r > 0 || r < -127 || r >= RSSI_STRONG) return;  // 强样本不参与渐离
    buf[head] = (int8_t)r;
    ts[head] = now;
    head = (uint8_t)((head + 1) % 6);
    if (n < 6) n++;
    while (n > 0) {
      uint8_t oldest = (uint8_t)((head - n + 12) % 6);
      if ((now - ts[oldest]) <= RSSI_TREND_WINDOW_MS) break;
      n--;
    }
  }
  bool gradualLeave() const {
    if (n < RSSI_TREND_MIN_N) return false;
    uint8_t start = (uint8_t)((head - n + 12) % 6);
    for (uint8_t i = 0; i + 1 < n; i++) {
      int a = buf[(start + i) % 6];
      int b = buf[(start + i + 1) % 6];
      if (b > a + RSSI_TREND_TOL_DB) return false;
    }
    int first = buf[start];
    int last = buf[(start + n - 1) % 6];
    if (first - last < RSSI_TREND_DROP_DB) return false;
    if ((ts[(start + n - 1) % 6] - ts[start]) > RSSI_TREND_WINDOW_MS)
      return false;
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
static bool gEverHadSignal = false;
static uint32_t gNoSigSince = 0; // 0=当前有信号

static void observeSignal(bool hasSignal, int rssi) {
  const uint32_t now = millis();
  sigLogPush(hasSignal ? rssi : -127);
  gLastSigRssi = hasSignal ? rssi : -127;
  gBt.recordTs(hasSignal ? (int16_t)rssi : (int16_t)-127);
  if (hasSignal) {
    gNoSigSince = 0;
    gEverHadSignal = true;
    if (rssi >= RSSI_STRONG) {
      // 门口强：滑动窗口累计（多径 强/弱 交替不得清零）
      gLastStrongMs = now;
      gLostSinceStrongMs = 0;
      gStrongAfterOpen = true;
      clearLeaveQual("回到强信号");
      gRssiTrend.clear();
      gHadDoorStrong = true;
      if (gStrongStreakSince == 0 ||
          millisReached(now, gStrongStreakSince + OPEN_STRONG_WINDOW_MS)) {
        gStrongStreakSince = now;
        gFirstStrongInWinMs = now;
        gStrongStreak = 0;
      }
      if (gStrongStreak < 255) gStrongStreak++;
      if (gStrongStreak >= OPEN_STRONG_STREAK &&
          millisReached(now, gFirstStrongInWinMs + OPEN_STRONG_HOLD_MS)) {
        if (!gStrongOpenReady) {
          Serial.printf(
              "[FSM] 门口强信号窗口达标 streak=%d win=%ums inGarage=%d → 允许自动开\n",
              (int)gStrongStreak, (unsigned)OPEN_STRONG_WINDOW_MS,
              (int)gInGarage);
        }
        gStrongOpenReady = true;
        if (gInGarage) {
          gInGarage = false;
          Serial.println("[FSM] 门口强信号累计达标 → 出库/回场，解除库内锁");
        }
      }
      return;  // 强样本不进渐离趋势
    }
    // 弱信号：不清空窗口内已累计的强次数（回库时 -65/-90 交替很常见）
    // 仅当窗口过期（最近强已超 OPEN_STRONG_WINDOW_MS）才取消开锁
    if (gLastStrongMs == 0 ||
        millisReached(now, gLastStrongMs + OPEN_STRONG_WINDOW_MS)) {
      gStrongStreak = 0;
      gStrongStreakSince = 0;
      gFirstStrongInWinMs = 0;
      gStrongOpenReady = false;
    }
    gRssiTrend.push(rssi, now);
    if (gRssiTrend.gradualLeave()) {
      if (!gLeaveQual) {
        Serial.printf("[FSM] 离场趋势合格 rssi=%d（≥%d 点单调变弱） strongAfter=%d\n",
                      rssi, (int)RSSI_TREND_MIN_N, (int)gStrongAfterOpen);
        gRssiTrend.dump();
        sigLogShip("leaveQual", rssi);
      }
      gLeaveQual = true;
    }
  } else {
    // 丢失：仅窗口过期才取消强开锁（短 miss 不打断门口累计）
    if (gLastStrongMs == 0 ||
        millisReached(now, gLastStrongMs + OPEN_STRONG_WINDOW_MS)) {
      gStrongStreak = 0;
      gStrongStreakSince = 0;
      gFirstStrongInWinMs = 0;
      gStrongOpenReady = false;
    }
    if (gNoSigSince == 0) gNoSigSince = now;
    if (gLastStrongMs != 0 && gLostSinceStrongMs == 0) {
      gLostSinceStrongMs = now;
      Serial.printf("[FSM] 门口强后信号丢失 t=%u\n", (unsigned)now);
    }
    if (millisReached(now, gNoSigSince + RSSI_TRUE_SILENT_MS) && !gTrueNo) {
      gTrueNo = true;
      // 长静默：真无确认；库内锁仍保留（弱路径开门看 gInGarage）
      clearLeaveQual("真无确认");
      gRssiTrend.clear();
      Serial.println("[FSM] 真无确认（弱路径开门仍受库内/抑制约束）");
    }
    if (!gStrongAfterOpen && gLeaveQual) {
      clearLeaveQual("门外消失且未进库");
    }
  }

  // 强后弱/丢满 OUT_IN_GARAGE_SILENT_MS → 车在库内（熄火后蓝牙仍亮也算）
  if (gLastStrongMs != 0 && millisReached(now, gLastStrongMs + OUT_IN_GARAGE_SILENT_MS)) {
    if (!gInGarage) {
      gInGarage = true;
      Serial.println("[FSM] 强信号后持续弱/丢 → 判定车在库内（禁弱路径自动开）");
    }
  }
}

// 门外安装关门：
//  1) 主路径：门口强信号后丢失满 OUT_CLOSE_SILENT_MS（开走/出库、入库）
//  2) 旁路：渐离趋势 + 开门后见过强 + 连续偏远
static bool shouldCloseBySignal(bool hasSignal, bool isFar) {
  if (!gCloseArmed) return false;
  const uint32_t now = millis();
  // 开走关门：强→丢失满时长（车离开门口 RF 区，或入库后人离开车库）
  if (gHadDoorStrong && gLostSinceStrongMs != 0 && !hasSignal) {
    if (millisReached(now, gLostSinceStrongMs + OUT_CLOSE_SILENT_MS)) {
      return true;
    }
  }
  // 旁路：仍有信号但已偏远 + 渐离合格（出门后未立刻丢扫到）
  if (gLeaveQual && gStrongAfterOpen && isFar) return true;
  return false;
}

// isFar 防抖：-90 凹点/跳动一次不算离场
static uint8_t gFarStreak = 0;
static bool debounceFar(bool hasSignal, int rssi) {
  if (!hasSignal) {
    gFarStreak = 0;
    return false;
  }
  if (rssi <= RSSI_FAR_CLOSE) {
    if (gFarStreak < 255) gFarStreak++;
  } else {
    gFarStreak = 0;
  }
  return gFarStreak >= RSSI_FAR_MIN_STREAK;
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
      Serial.println("[BOOT] STA ip=" + gWeb.staIp() + " ota=ip");
    }
  } else {
    if (gForceApArmed && !gForceApHandled && (now - gBootHoldStartMs) >= 1500 &&
        (now - gBootHoldStartMs) < 3000) {
      Serial.println("[BOOT] BOOT released too early (<3s), skip force AP");
    }
    gForceApArmed = false;
  }
}

// ===== 数据面看门狗：sta=1 却连续网络层失败 → 僵尸关联，强制重连自愈 =====
// 背景：dda0 曾在路由器侧网络正常时静默 33 分钟——WiFi.status() 一直报已连接，
// 但 DNS/connect 全失败（deauth 漏收/DHCP 黑洞）。loopSta 只要 WL_CONNECTED 就
// 提前 return，永远不会自救。这里补上数据面校验：发送结果说"网络层失败"才动手。
// 20261002 弱网退避：dda0 实测 120s 限频下仍 76 次/2天 强制重连——每次重连的
// 扫描+DHCP 瞬时吃掉最后几 KB（8BIT 总水位 p50 仅 7.5KB），是 BTU 4112 竞态
// 的头号尖峰源。改为 120s→300s→600s 封顶逐级退避；HTTP 收到成功（streak 归零）
// 即降回 1 级。踢完不再主动清 streak：留着它才能升级，成功自然会清。
static void serviceStaDataWatchdog() {
  static uint32_t lastKickMs = 0;
  static uint8_t kickLevel = 0;
  if (!gWeb.staConnected()) return;  // 真断开由 loopSta 节流重连，不归这里管
  if (httpClientWebBusy()) return;   // 本地网页正占射频发大响应：失败多半是自己造成的，别拆 WiFi
  int streak = httpClientNetFailStreak();
  if (streak == 0) {
    kickLevel = 0;  // 有成功 → 退避等级归零
    return;
  }
  if (streak < HTTP_NET_FAIL_KICK) return;
  static const uint32_t kKickIntervalsMs[] = {120000UL, 300000UL, 600000UL};
  uint32_t now = millis();
  if (lastKickMs != 0 && (now - lastKickMs) < kKickIntervalsMs[kickLevel]) {
    return;  // 限频防抖（逐级拉长）
  }
  lastKickMs = now;
  if (kickLevel < 2) kickLevel++;
  logShipf("[WEB] datagate sta=1 netfail=%d lvl=%u -> force STA reconnect",
           streak, (unsigned)kickLevel);
  gWeb.forceStaReconnect();
}

// ===== 堆损坏侦查（20260927 panic 定位）=====
// 崩溃实录：Bluedroid search_devices_copy_cb 里 osi_malloc(524) 返 NULL 后
// memset(NULL)——但崩溃前 1s 心跳 maxblk=11252，524 不该分不出来 → free list
// 疑似被写坏。三件套：分配失败钩子（谁、分多大、在哪失败）+ 周期完整性自检
// （损坏出现在哪 2 秒窗口）+ 任务栈水位（找栈溢出写坏堆的元凶）。
static volatile uint32_t gHeapFailN = 0;
// 共用气囊（remote_ota 的 OTA_RESERVE_SIZE 预留）归还请求：钩子里只置位，
// 实际 free 必须在 loop 做——钩子在分配路径里严禁碰堆
static volatile bool gResGiveReq = false;
// 失败事件只在钩子里记字段（无锁、不 printf），完整诊断在 loop 上下文打印
struct HeapFailEvt {
  volatile uint32_t seq;
  uint32_t size;
  uint32_t caps;
  void* ra0;
  void* ra1;
  void* ra2;
  void* ra3;
  void* ra4;
  void* ra5;
  char task[16];
  char fn[24];
};
static HeapFailEvt gFailEvt;

static void onAllocFailed(size_t size, uint32_t caps, const char* fn) {
  // 分配路径里被调：严禁再走堆/分配，防重入防死锁
  static volatile bool inHook = false;
  if (inHook) return;
  inHook = true;
  uint32_t n = ++gHeapFailN;
  // 精确 BTU 失败计数：始终取任务名（不依赖抽样），钩子内只累加
  {
    const char* tnm = "?";
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (h) {
      const char* nm = pcTaskGetName(h);
      if (nm) tnm = nm;
    }
    ClassicTracker::noteAllocFail(size, tnm);
  }
  // 气囊容量以内的失败 → 请 loop 归还共用气囊。
  // 旧阈值 4096 盖不住 BTU inquiry 的 4112B（dda0 max8=4084 实锤），
  // 导致 BTU 失败根本不触发归还；现改为 OTA_RESERVE_SIZE(4352)。
  // 故意不做「largest 不够就跳过 inquiry」——那会推迟自动开门。
  if (size <= OTA_RESERVE_SIZE) gResGiveReq = true;
  if (n <= 16 || (n & 255) == 0) {
    gFailEvt.size = (uint32_t)size;
    gFailEvt.caps = (uint32_t)caps;
    gFailEvt.ra0 = __builtin_return_address(0);
    gFailEvt.ra1 = __builtin_return_address(1);
    // ra2/ra3 穿过 heap_caps_* 公共层，指向真正调用 malloc/realloc 的代码
    gFailEvt.ra2 = __builtin_return_address(2);
    gFailEvt.ra3 = __builtin_return_address(3);
    // ra4/ra5: malloc 的调用者（String/lwIP/业务代码，真正要抓的层）
    gFailEvt.ra4 = __builtin_return_address(4);
    gFailEvt.ra5 = __builtin_return_address(5);
    const char* fnm = fn ? fn : "?";
    strncpy(gFailEvt.fn, fnm, sizeof(gFailEvt.fn) - 1);
    gFailEvt.fn[sizeof(gFailEvt.fn) - 1] = '\0';
    gFailEvt.task[0] = '?';
    gFailEvt.task[1] = '\0';
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (h) {
      const char* nm = pcTaskGetName(h);
      if (nm) {
        strncpy(gFailEvt.task, nm, sizeof(gFailEvt.task) - 1);
        gFailEvt.task[sizeof(gFailEvt.task) - 1] = '\0';
      }
    }
    gFailEvt.seq = n;  // 最后写：loop 见 seq 变化才消费
  }
  inHook = false;
}

static void serviceHeapDiag() {
  static uint32_t lastChk = 0, lastStack = 0;
  uint32_t now = millis();
  if (now - lastChk >= 2000) {
    lastChk = now;
    if (!heap_caps_check_integrity_all(false)) {
      Serial.println("[HEAP] INTEGRITY FAIL — free list 已损坏!");
      heap_caps_check_integrity_all(true);  // 第二遍打印细节
    }
  }
  // 消费最近一次分配失败：分池统计 + 调用者返回地址（addr2line 定位）
  {
    static uint32_t lastSeen = 0;
    uint32_t seq = gFailEvt.seq;
    if (seq != lastSeen) {
      lastSeen = seq;
      multi_heap_info_t iCap, i8bit;
      heap_caps_get_info(&iCap, gFailEvt.caps);
      heap_caps_get_info(&i8bit, MALLOC_CAP_8BIT);
      // logShipf = 串口 + VPS 双通道：侦查数据不插 USB 也能从设备日志看
      // 格式: free/big 成对（8BIT 大小 vs 失败 caps 口径大小）
      logShipf("[HEAPFAIL] #%u sz=%u t=%s big8=%u ra4=%p ra5=%p",
               (unsigned)seq, (unsigned)gFailEvt.size, gFailEvt.task,
               (unsigned)i8bit.largest_free_block, gFailEvt.ra4,
               gFailEvt.ra5);
    }
  }
  // 精确 BTU 失败日志：计数变化就打，不依赖 HEAPFAIL 抽样
  {
    static uint32_t lastBtu = 0;
    uint32_t btu = ClassicTracker::btuFailCount();
    if (btu != lastBtu) {
      lastBtu = btu;
      multi_heap_info_t i8bit;
      heap_caps_get_info(&i8bit, MALLOC_CAP_8BIT);
      logShipf("[BTUFAIL] n=%u big8=%u thin=%u inq=%u fail=%u",
               (unsigned)btu, (unsigned)i8bit.largest_free_block,
               (unsigned)ClassicTracker::thinCount(),
               (unsigned)ClassicTracker::inqCount(), (unsigned)gHeapFailN);
    }
  }
  // 共用气囊：钩子已置位 → 这里归还给堆（WiFi 下一帧 2308B / BTU 4112B 就能成）
  if (gResGiveReq) {
    gResGiveReq = false;
    remoteOtaReserveGive();
  }
  // BTU 专用应急堆：BTU 分配失败钩子置位 → 这里 free（钩子内严禁碰堆）
  ClassicTracker::serviceBtuReserve(gWeb.staConnected());
  // 每 10s 分池水位：哪个 caps 口径在「饿」
  static uint32_t lastPool = 0;
  if (now - lastPool >= 10000) {
    lastPool = now;
    // 气囊收回：失败风暴过去、池子重新宽裕（largest8≥OTA_RESERVE_REARM_MIN
    // ≈6400）才收，且最多 60s 一次——防止"收回→又被吃→再收"边界抖动刷日志
    // 经典跟踪开启时禁止 rearm：气囊归还后留给 BTU inquiry 4112B / WiFi 2308B；
    // 否则「give→短暂 maxblk 变大→rearm 抢回→BTU 再 fail」会把 fail 刷上去。
    // OTA begin 前本就有让出路径，跟踪期不占这块。
    static uint32_t lastRearmMs = 0;
    if (!gBt.autoTrack() && !remoteOtaReserveHeld() && !gResGiveReq &&
        (now - lastRearmMs) >= 60000UL) {
      lastRearmMs = now;  // 成败都计时：池子不宽裕时也不用每 10s 白跑 malloc
      remoteOtaReserveRearm();
    }
    // 不再看串口缓冲：logShipf 自带串口+VPS 双通道，门闩会让 VPS 侧漏数据
    {
      multi_heap_info_t iDef, i18, i8, iIn;
      heap_caps_get_info(&iDef, MALLOC_CAP_DEFAULT);
      heap_caps_get_info(&i18, MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT);
      heap_caps_get_info(&i8, MALLOC_CAP_8BIT);
      heap_caps_get_info(&iIn, MALLOC_CAP_INTERNAL);
      // 每池格式: total_free/largest_free
      logShipf("[HEAPPOOL] DEF %u/%u INTDEF %u/%u 8BIT %u/%u INT %u/%u fail=%u",
               (unsigned)iDef.total_free_bytes,
               (unsigned)iDef.largest_free_block,
               (unsigned)i18.total_free_bytes,
               (unsigned)i18.largest_free_block,
               (unsigned)i8.total_free_bytes,
               (unsigned)i8.largest_free_block,
               (unsigned)iIn.total_free_bytes,
               (unsigned)iIn.largest_free_block, (unsigned)gHeapFailN);
      // 精确 BTU/thin 统计：与 HEAPFAIL 抽样无关，status 同步上报
      // mode: 0=IDLE(8s探针) 1=ACTIVE(5+1) —— 验收按时段分开统计用
      logShipf("[BTSTAT] inq=%u thin=%u btufail=%u fail=%u maxblk=%u mode=%u",
               (unsigned)ClassicTracker::inqCount(),
               (unsigned)ClassicTracker::thinCount(),
               (unsigned)ClassicTracker::btuFailCount(), (unsigned)gHeapFailN,
               (unsigned)i8.largest_free_block,
               (unsigned)(gBt.epochActive() ? 1 : 0));
    }
  }
  if (now - lastStack >= 10000) {
    lastStack = now;
    if (Serial.availableForWrite() > 256) {
      uint32_t loopHwm =
          (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
      TaskHandle_t nh = gNfc.taskHandle();
      uint32_t nfcHwm = nh
                            ? (uint32_t)uxTaskGetStackHighWaterMark(nh) *
                                  sizeof(StackType_t)
                            : 0;
      Serial.printf("[TASK] loop=%uB nfc=%uB httpWorker=%uB failN=%u\n",
                    loopHwm, nfcHwm, httpClientWorkerStackHwm(),
                    (unsigned)gHeapFailN);
    }
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
        // 二选一：仅经典模式起 SerialBT
        if (gWeb.trackMode() == TRACK_MODE_CLASSIC) {
          gBt.begin(gMac);
        }
        Serial.println("[CMD] MAC set+saved " + m);
      } else if (line == "wifi") {
        Serial.println("[CMD] AP " + gWeb.apSsid() +
                       (gWeb.apActive() ? " active" : " off") + " pass=" +
                       AP_PASSWORD);
        if (gWeb.apActive()) {
          Serial.println("[CMD] ap_ip=" + WiFi.softAPIP().toString());
        }
        Serial.println("[CMD] sta=" + String(gWeb.staConnected() ? "up" : "down") +
                       " ip=" + gWeb.staIp() + " name=" + gWeb.staHostname() +
                       " ota=off(vps)");
      } else if (line == "wifi off") {
        // 与网页一致：只关热点、保留 STA；BT 栈由 serviceBtStackInit 延时起
        gCfg.saveWifiEnabled(false);
        gWeb.stopAp();
        gBt.setInquiryPaused(false);
        Serial.println("[CMD] SoftAP OFF (STA kept); BT stack will start in ~1.5s");
      } else if (line == "wifi sta off") {
        gWeb.stopSta();
        Serial.println("[CMD] STA OFF (SoftAP unchanged)");
      } else if (line == "wifi sta on") {
        gWeb.startStaFromStore();
        Serial.println("[CMD] STA ON " + gWeb.staIp());
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
      } else if (line == "web on") {
        gCfg.saveWebUi(true);
        gWeb.setUiEnabled(true);
        Serial.println("[CMD] local web ON (80端口开)");
      } else if (line == "web off") {
        gCfg.saveWebUi(false);
        gWeb.setUiEnabled(false);
        Serial.println("[CMD] local web OFF (80端口关；远程/日志/OTA不受影响)");
      } else if (line == "logs flush") {
        logShipFlushNow();
        Serial.println("[CMD] logs flush queued pending=" +
                       String((unsigned)logShipPending()));
      } else if (line == "logs") {
        Serial.printf("[CMD] logship pending=%u last=%s\n",
                      (unsigned)logShipPending(), remoteOtaLastMsg());
      } else if (line == "ota check") {
        remoteOtaCheckNow();
        Serial.println("[CMD] ota check queued");
      } else if (line == "ota") {
        Serial.printf("[OTA] last=%s active=%d\n", remoteOtaLastMsg(),
                      (int)remoteOtaActive());
      } else if (line == "wifi status") {
        Serial.printf("[CMD] mode=%d ap=%s ip=%s sta=%d apmac=%s heap=%u bt=%d\n",
                      (int)WiFi.getMode(), gWeb.apSsid().c_str(),
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum(),
                      WiFi.softAPmacAddress().c_str(),
                      (unsigned)ESP.getFreeHeap(), (int)gBtStackInited);
        Serial.println("[CMD] sta=" + String(gWeb.staConnected() ? "up" : "down") +
                       " ip=" + gWeb.staIp() + " name=" + gWeb.staHostname() +
                       " ota=off(vps)" +
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
        // ble | ble 12 → 主动扫 10/12 秒（仅 BLE 模式）
        if (gWeb.trackMode() != TRACK_MODE_BLE || !gBleBond.begun()) {
          Serial.println("[CMD] BLE scan ignored (not BLE mode / stack off)");
        } else {
          uint32_t ms = 10000;
          int sp = line.indexOf(' ');
          if (sp > 0) {
            int sec = atoi(line.substring(sp + 1).c_str());
            if (sec >= 5 && sec <= 30) ms = (uint32_t)sec * 1000;
          }
          if (gWeb.trackMode() == TRACK_MODE_CLASSIC) {
            gBt.setAutoTrack(false);
            gBt.setInquiryPaused(true);
          }
          Serial.println("[CMD] BLE scan starting...");
          gBleScan.runScan(ms);
        }
      } else if (line == "bletrack on") {
        if (gWeb.trackMode() != TRACK_MODE_BLE) {
          Serial.println("[CMD] bletrack ignored (classic mode)");
        } else {
          gBleScan.setTrack(true);
          Serial.println("[CMD] BLE IRK track ON (paired phone only)");
        }
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
        if (gWeb.trackMode() != TRACK_MODE_BLE) {
          Serial.println("[CMD] blepair ignored (classic mode)");
        } else {
          gBleBond.requestOpenPairing(90000);
        }
      } else if (line == "blepair off") {
        gBleBond.closePairingWindow("manual");
      } else if (line.startsWith("blepair ")) {
        if (gWeb.trackMode() != TRACK_MODE_BLE) {
          Serial.println("[CMD] blepair ignored (classic mode)");
        } else {
          int sec = atoi(line.substring(8).c_str());
          if (sec <= 0)
            gBleBond.requestOpenPairing(0);
          else
            gBleBond.requestOpenPairing((uint32_t)sec * 1000);
        }
      } else if (line == "bleunpair") {
        gBleBond.clearBond("serial");
      } else if (line.startsWith("blepin ")) {
        gBleBond.setPairingPin(line.substring(7));
      } else if (line == "autotrack on") {
        if (gWeb.trackMode() != TRACK_MODE_CLASSIC) {
          Serial.println("[CMD] autotrack ignored (BLE mode — classic Inquiry off)");
        } else {
          gBt.setAutoTrack(true);
          gCfg.saveAutoTrack(true);
          Serial.println("[CMD] autotrack ON (periodic inquiry, saved)");
        }
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
      } else if (line.startsWith("rfpin")) {
        // rfpin high | rfpin low | rfpin pulse [ms] —— 直接操控发射脚 GPIO
        // （先把引脚从 RMT 矩阵夺回普通 GPIO，测完需重启才能再用 rfplay）
        String rest = line.substring(5);
        rest.trim();
        if (rest == "high") {
          gRf.pinGpioLevel(true);
        } else if (rest == "low") {
          gRf.pinGpioLevel(false);
        } else if (rest.startsWith("pulse")) {
          uint32_t ms = 1000;
          int sp = rest.indexOf(' ');
          if (sp > 0) {
            long v = rest.substring(sp + 1).toInt();
            if (v >= 10 && v <= 5000) ms = (uint32_t)v;
          }
          gRf.pinGpioPulse(ms);
        } else {
          Serial.println("[RF] 用法: rfpin high | rfpin low | rfpin pulse [ms]");
        }
      } else if (line.startsWith("rfsoft ")) {
        // rfsoft 0|1|2|3 —— 强制旧同步 bit-bang 路径发码（绕过 RMT）
        int idx = rfKeyIndexFromArg(line.substring(7));
        if (idx < 0) {
          Serial.println("[RF] 用法: rfsoft 0|1|2|3  或 open/close/stop/lock");
        } else {
          gRf.playKeySoft(idx);
        }
      } else if (line.startsWith("rfmon")) {
        // rfmon [0-3] —— 异步发码并采样 GPIO26，验证波形真到引脚
        String rest = line.substring(5);
        rest.trim();
        int idx = rest.length() ? rfKeyIndexFromArg(rest) : RF_KEY_CLOSE;
        if (idx < 0) {
          Serial.println("[RF] 用法: rfmon [0-3]（默认 1=关门）");
        } else {
          gRf.playKeyMonitor(idx);
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
      } else if (line == "rfexport") {
        for (int i = 0; i < RF_KEY_COUNT; i++) gRf.exportKeyCsv(i);
        Serial.println("[RF] export done");
      } else if (line == "gpio17" || line == "i2cscan" || line == "i2cscan2") {
        // 推拉测试只给 gpio17：i2cscan 前不要动 SCL，否则会把 PN532 弄挂
        if (line == "gpio17") {
          // 先停 NFC + 断开 I2C 矩阵，否则对侧任务/Wire 会把 SCL 按住，测不准
          gNfc.setSuspended(true);
          Wire.end();
          gpio_reset_pin((gpio_num_t)PIN_NFC_SCL);
          gpio_reset_pin((gpio_num_t)PIN_NFC_SDA);
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
          gNfc.setSuspended(false);
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
          Wire.begin(sdaP, sclP, (uint32_t)NFC_I2C_HZ);
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
        Wire.begin(PIN_NFC_SDA, PIN_NFC_SCL, (uint32_t)NFC_I2C_HZ);
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
      } else if (line == "busfree" || line == "busreset") {
        // 松 Wire + 强制脚回 GPIO：I2C 矩阵仍挂着时 SCL 会被外设按在 0
        gNfc.setSuspended(true);
        Wire.end();
        gpio_reset_pin((gpio_num_t)PIN_NFC_SDA);
        gpio_reset_pin((gpio_num_t)PIN_NFC_SCL);
        pinMode(PIN_NFC_SDA, INPUT_PULLUP);
        pinMode(PIN_NFC_SCL, INPUT_PULLUP);
        delay(5);
        Serial.printf("[BUS] %s SDA16=%d SCL17=%d\n", line.c_str(),
                      digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL));
        if (line == "busreset") {
          delay(300);
          Serial.printf("[BUS] after300ms SDA16=%d SCL17=%d\n",
                        digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL));
        }
        gNfc.setSuspended(false);
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
        gNfc.setSuspended(true);  // 避免与 nfc 任务并发摸 I2C
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
        gNfc.setSuspended(false);
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
            "cmds: status | open | close | rfcap | rfstop | rflearn 0-3 | rfplay 0-3 | rfsoft 0-3 | rfmon [0-3] | rfpin high|low|pulse | rfauto on|off (max 2min) | rfloop 0 | rfbench 0 6 | rfcloop | rfcarrier | rfkeys | "
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

// OTA 的三条软复位路径（成功/失败/BT拆栈恢复）在 ESP.restart() 前统一回调这里
static void otaPreResetNfc() { gNfc.stopForOta(); }

void setup() {
  Serial.begin(SerialBaud);
  delay(200);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" Garage Door Controller  P0.1");
  Serial.println(" SoftAP web config + gradual BT");
  Serial.println("========================================");

  // 防 NVS 自动恢复 STA 与 BT 并行起导致 SW_CPU_RESET
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  Serial.println("[BOOT] WiFi forced OFF at boot (will start later if needed)");
  // 事件时间戳：NTP 对时后每条上云日志带「发生时间」而不是上传时间
  // （离线积压补传时两者差可达几分钟）。同步前的行不打戳，
  // VPS 按接收时间兜底——好过打一个错误的 1970 时间。时区固定东八区无夏令时。
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  logShipBegin();  // 必须在 early SCL 日志前，否则 s_len=0 会冲掉
  crashSnapBegin();  // 崩溃快照（RTC noinit）：上一轮现场由 crashSnapReport 上报
  // 复位原因只打串口、VPS 看不到（7 次静默重启无从查），开机补报是唯一定案线索：
  // 1=掉电/上电 3=软件重启 4=panic 5=INT_WDT 6=Task_WDT 9=brownout
  logShipf("[BOOT] rst=%d t=%ums", (int)esp_reset_reason(), (unsigned)millis());
  // 崩溃现场（RTC noinit 跨复位保留）：仅 PANIC/WDT/BROWNOUT 时上报
  crashSnapReport((int)esp_reset_reason());
  // 20260930 1388 实测：开机 hold 大气囊（12KB）后稳态 maxblk 掉到 1908，
  // WiFi 2308 / BTU 4112 一起 fail，fail 速率反升到 ~2.4/s。
  // OTA begin 本就有「射频下电 → 池子变大」路径，跟踪期不再开机占大块。
  // remoteOtaHold4k();  // 已停用：见上

  // 最早期测 SDA/SCL 电平（尚未碰 I2C/WiFi/BT）——排除软件把脚拉死
  gpio_reset_pin((gpio_num_t)PIN_NFC_SDA);
  gpio_reset_pin((gpio_num_t)PIN_NFC_SCL);
  pinMode(PIN_NFC_SDA, INPUT_PULLUP);
  pinMode(PIN_NFC_SCL, INPUT_PULLUP);
  delay(2);
  Serial.printf("[BOOT] early SDA16=%d SCL17=%d t=%ums\n",
                digitalRead(PIN_NFC_SDA), digitalRead(PIN_NFC_SCL),
                (unsigned)millis());
  if (!digitalRead(PIN_NFC_SCL)) {
    // 软件尚未碰 Wire：仍为 0 则是外部（PN532/短路），不是 I2C 矩阵
    Serial.printf("[BOOT] early SCL=0 → 脚已 gpio_reset+pullup，外部拉住 t=%ums\n",
                  (unsigned)millis());
    logShipf("[BOOT] early SCL=0 t=%ums", (unsigned)millis());
  } else {
    logShipf("[BOOT] early SCL=1 t=%ums", (unsigned)millis());
  }

  gDoor.begin();
  // 尽早钳位 RF TX，避免上电到 gRf.begin 之间脚位浮空乱发
  pinMode(PIN_RF_TX, OUTPUT);
  digitalWrite(PIN_RF_TX, LOW);
  remoteCmdSetHandler(onRemoteCmd);
  remoteCmdSetMemTrim([]() { gBleScan.releaseMemory(); });
  httpClientBegin(httpBtRadioBusy);  // 异步 HTTP 任务（loop 不再阻塞等网络）
  heap_caps_register_failed_alloc_callback(onAllocFailed);  // 堆侦查：分配失败留痕
  gCfg.begin();
  remoteCmdBegin(&gCfg);
  // logShipBegin 已在 early SCL 日志前调用，此处再 begin 会清掉已入队日志
  statusReportBegin();
  remoteOtaBegin(&gCfg);
  remoteOtaSetBusyHook([](bool on) {
    gOtaActive = on;
    if (on) {
      // 挂起 + 清总线：只挂起不清理会把 PN532 留在半截 I2C 事务里，
      // 软复位后它拉住 SCL（OTA 软重启 3 次卡死 2 次的根因）。
      // stopForOta 只做纯 GPIO（无 ACK 等待/超时），不会卡死 OTA。
      gNfc.stopForOta();
      if (gBtStackInited) {
        gBt.setInquiryPaused(true);  // 内部会 cancel discovery，非阻塞
      }
    } else {
      gNfc.setSuspended(false);
      if (gBtStackInited) gBt.setInquiryPaused(false);
      if (gNfc.ok()) gNfc.setListen(true);
      else gNfc.kickRecover();
    }
  });
  remoteOtaSetPreResetHook(otaPreResetNfc);
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
  logShipf("[BOOT] wifiOn=%d debug_boot=%d max8=%u",
           wifiOn ? 1 : 0, WIFI_DEBUG_BOOT_ON ? 1 : 0,
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  // NFC：上电约 5s 后自动 init（原先永久 deferred，断电后刷卡会失效）
  Serial.println("[BOOT] NFC auto-init scheduled (~5s)");
  gNfc.begin(PIN_NFC_SDA, PIN_NFC_SCL, PIN_NFC_IRQ);
  gNfc.setCardHandler([](const String& uid) {
    const bool auth = gNfc.isAuthorized(uid);
    if (auth) {
      gDoor.requestManualToggle(OpenSource::NFC);
      logShipf("[NFC] card: %s authorized → RF", uid.c_str());
    } else if (gNfc.authUid().length() == 0) {
      logShipf("[NFC] card: %s unregistered", uid.c_str());
    } else {
      logShipf("[NFC] card: %s unauthorized", uid.c_str());
    }
    Serial.printf("[NFC] card cb uid=%s auth=%d\n", uid.c_str(), (int)auth);
  });
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

  // SoftAP=0：先 BT 栈，再 STA（并行会 SW_CPU_RESET）
  if (wifiOn) {
    logShipf("[BOOT] SoftAP on → BT/BLE 栈推迟到关热点后；NFC 空闲时自动 init");
  } else {
    logShipf("[BOOT] BT first (no STA), then start STA...");
    delay(200);
    initBtStacks();
    logShipf("[BOOT] after initBtStacks, before STA... max8=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    delay(1500);
    if (gWeb.staConfigured()) {
      logShipf("[BOOT] startStaFromStore...");
      gWeb.startStaFromStore();
      logShipf("[BOOT] startStaFromStore returned");
    } else {
      logShipf("[BOOT] No STA configured.");
    }
  }

  logShipf("[BOOT] ready. fw=%s wifi=%d bt_inited=%d max8=%u",
           FW_VERSION, wifiOn ? 1 : 0, (int)(gBtStackInited ? 1 : 0),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
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
      Serial.println("[BOOT] 已配家庭 Wi‑Fi，将连 STA（OTA 用 STA IP）");
    }
  } else {
    Serial.println("[BOOT] SoftAP off (wifi_on=0) — will NOT auto-start AP again");
    Serial.println("[BOOT] Web: wait STA IP printed as [WEB] STA HTTP up at http://x.x.x.x/");
    Serial.println("[BOOT] To open AP once: hold BOOT 3s, or serial: wifi on");
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
  // SCL/SDA 心跳：边沿必须限流。NFC 任务在 I2C 时脚位高速翻转，
  // loop 每轮 digitalRead 都会当成「边沿」打串口 → Serial 堵死 → poll/状态 45s 离线。
  {
    static int lastSda = -1, lastScl = -1;
    static uint32_t lastBusLog = 0;
    int sda = digitalRead(PIN_NFC_SDA);
    int scl = digitalRead(PIN_NFC_SCL);
    uint32_t now = millis();
    uint32_t busPeriod = gWeb.apActive() ? 5000 : 2000;
    bool edge = (sda != lastSda || scl != lastScl);
    if (edge) {
      lastSda = sda;
      lastScl = scl;
      // 异常（脚被拉低）稍密；正常跳变最多 1s 一条
      uint32_t edgeGap = (sda == 0 || scl == 0) ? 300UL : 1000UL;
      if (now - lastBusLog >= edgeGap) {
        Serial.printf("[BUS] t=%ums SDA16=%d SCL17=%d%s\n", (unsigned)now, sda,
                      scl, (scl ? " (idle high)" : " (SCL LOW)"));
        lastBusLog = now;
      }
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
  serviceRfTxSafety();
  // 先跑蓝牙调度，再跑 NFC：避免 I2C 抢在 Inquiry/BLE 之前占满 loop
  serviceBtStackInit();
  if (gBtStackInited) {
    // 二选一：经典模式才跑 Inquiry 调度；BLE 模式 gBt 未 begin，loop 空转即可
    if (gWeb.trackMode() == TRACK_MODE_CLASSIC) {
      // 门态喂入：开门沿 → ACTIVE 纪元（晨间出库的信号窗由事件锚定）
      gBt.setDoorOpen(gDoor.doorState() == DoorState::OPEN);
      gBt.loop();
    }
    gBleBond.service();  // 内部 begun_ 门闩：经典模式直接 return
  }

  // 远程令：蓝牙忙不发 HTTPS；放在 NFC 之后，避免 TLS 抢贴卡窗口
  // （见下方 NFC 块之后调用 remoteCmdService）

  // ===== NFC 异步：I2C 在独立任务，loop 只收事件 =====
  {
    const bool apOn = gWeb.apActive();
    const bool apClient = apOn && WiFi.softAPgetStationNum() > 0;
    gNfc.setSuspended(gOtaActive);
    if (!gOtaActive) {
      static bool nfcWasQuiet = false;
      if (apClient != nfcWasQuiet) {
        nfcWasQuiet = apClient;
        if (!apClient && !gNfc.ok()) gNfc.kickRecover();
      }
      if (apClient) {
        gNfc.setPollGapMs(800);
      } else if (apOn) {
        gNfc.setPollGapMs(500);
      } else {
        gNfc.setPollGapMs(NFC_POLL_GAP_BT_TRACK_MS);
      }
      if (gNfc.ok() && !gNfc.listen()) gNfc.setListen(true);
      gNfc.service();  // 取读卡事件 → 开门回调
    }
  }

  // poll：inquiry 空闲且堆不薄时才发；log/status：inquiry/保护窗/堆薄时不发
  {
    gRf.service();  // 异步 RF 发射到点后清 busy
    const bool btBusy =
        gBtStackInited && (gBt.inquiryBusy() || gBleScan.busy());
    // 弱网 dda0：maxblk 常在 4852↔1500 震荡；堆薄时连 poll 也停，
    // 避免 WiFi 数据面继续把连续块打碎到 <4112（BTU 4112）
    // 20261001 1743 实锤：BT air/BTU reserve 每轮 inquiry 后主动 hold 4608，
    // 把 maxblk 压到 4084 <4600 → heapThin 恒真 → 日志/status 全程被静默、
    // 环挤满丢行。hold 是设计行为不是碎片——把钉住的字节加回再判薄。
    const uint32_t maxblkNow =
        heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) +
        ClassicTracker::pinnedHeapBytes();
    const bool heapThin = maxblkNow < 4600;
    // ===== Round-3 门控（20261002 A/B 实锤后的定稿）=====
    // 0800(门4600): 91 探针 0 挂但日志闷 11 分钟 —— 4600 是"降级时闭嘴
    // 让堆自愈"的保护；0809(门2600): 日志通但 76% 挂 —— 2600 会让 HTTP 在
    // 4084 残堆上持续咀嚼，永远合并不回去。所以门限回 4600，另配：
    // ① 卡死破除器：门连续关满 60s 强制放行一轮（观测最坏断 60s，不是21min）
    // ② ACTIVE 整窗豁免（窗就是排水口）
    // ③ 探针前排水（预静默+等在飞，见 classic_tracker）从源头减少咀嚼
    const bool thinBlock = heapThin && !gBt.inHttpWindow();
    bool btQuiet =
        gBtStackInited && (btBusy || gBt.btQuietForHttp() || thinBlock);
    static uint32_t quietSinceMs = 0;
    const uint32_t qNow = millis();
    if (btQuiet) {
      if (quietSinceMs == 0) quietSinceMs = qNow;
      if ((qNow - quietSinceMs) >= 60000UL) btQuiet = false;  // 卡死破除
    } else {
      quietSinceMs = 0;
    }
    // 堆从 thin 恢复：立刻 poke log_ship，避免弱网下 30s 周期一直错过空窗
    static bool wasHeapThin = false;
    if (heapThin) {
      wasHeapThin = true;
    } else if (wasHeapThin) {
      wasHeapThin = false;
      logShipPoke();
    }
    remoteCmdService(btQuiet, gWeb.staConnected());
    logShipService(btQuiet, gWeb.staConnected());
    remoteOtaService(btBusy, gWeb.staConnected());
    serviceStaDataWatchdog();
    serviceHeapDiag();
    // 崩溃快照：每 1s 抓一次当前任务调用栈到 RTC noinit 段。
    // panic 时无法执行用户代码（panic_abort 结尾就是 break），拿不到崩溃瞬间
    // 的栈；只能靠周期采样，1s 粒度足以定位"卡在哪个函数"。
    {
      static uint32_t lastSnap = 0;
      uint32_t nowSnap = millis();
      if ((int32_t)(nowSnap - lastSnap) >= 1000) {
        lastSnap = nowSnap;
        crashSnapCapture();
      }
    }
    {
      StatusBits sb;
      sb.nfcOk = gNfc.ok();
      sb.nfcDeferred = gNfc.deferred();
      sb.nfcAbsent = gNfc.absent();
      sb.nfcListen = gNfc.listen();
      sb.webUp = gWeb.uiEnabled();  // 本地网页开关（VPS 控制台接管后通常为关）
      sb.sta = gWeb.staConnected();
      sb.ap = gWeb.apActive();
      sb.rfOpen = gRf.keyValid(RF_KEY_OPEN);
      sb.rfClose = gRf.keyValid(RF_KEY_CLOSE);
      sb.rfTxBusy = gRf.txBusy();
      sb.remoteOn = remoteCmdEnabled();
      sb.door = (int)gDoor.doorState();
      sb.rssi = gWeb.staConnected() ? WiFi.RSSI() : 0;
      // 心跳/控制台改 DEF 池口径：ESP.getFreeHeap/getMaxAllocHeap 是
      // INTERNAL 口径（含 11KB malloc 摸不到的死块），一直误导排障；
      // DEF 才是 malloc 真正在用的池（[HEAPPOOL] 有全量分池）
      sb.heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
      sb.maxblk = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
      sb.uptimeMs = millis();
      // ===== 控制台配置回显 =====
      gWeb.staIp().toCharArray(sb.staIp, sizeof(sb.staIp));
      strlcpy(sb.mac, gMac, sizeof(sb.mac));
      sb.trackMode = gWeb.trackMode();
      sb.autoTrack = gBt.autoTrack();
      sb.pairOpen = gBleBond.pairingOpen();
      sb.pairHasPin = gBleBond.hasPasskey();
      sb.bleRssi = gBleScan.matchRssi();
      gBleScan.matchLabel().toCharArray(sb.bleLab, sizeof(sb.bleLab));
      sb.carRssi = gBt.lastRssi();
      sb.trend = (int)gBt.trend();
      sb.heapFailN = gHeapFailN;
      sb.btuFailN = ClassicTracker::btuFailCount();
      sb.thinN = ClassicTracker::thinCount();
      sb.inqN = ClassicTracker::inqCount();
#ifdef DEVICE_ROLE
      sb.role = DEVICE_ROLE;
#endif
      statusReportSetBits(sb);
      statusReportService(btQuiet, gWeb.staConnected());
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
    if (gBtStackInited && gWeb.trackMode() == TRACK_MODE_BLE &&
        gBleBond.begun() && gBleScan.trackOn() && !wifiClient) {
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
      bool isFar = false;

      if (scanJustFinished) {
        isFar = debounceFar(hasSignal, hasSignal ? r : 0);
        observeSignal(hasSignal, hasSignal ? r : 0);
      }

      if (scanJustFinished) {
        Serial.printf(
            "[BLE] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d inGar=%d strongOK=%d phase=%d\n",
            r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
            (int)gLeaveQual, (int)gInGarage, (int)gStrongOpenReady, (int)phase);
        // 关键变化进日志环（VPS 可回放 BLE RSSI 曲线）；与经典路径的节流一致
        static int lastShipBleRssi = -999;
        static int lastShipBleSeen = -1;
        if ((int)seen != lastShipBleSeen ||
            (seen && abs(r - lastShipBleRssi) >= 5)) {
          lastShipBleSeen = (int)seen;
          lastShipBleRssi = seen ? r : -999;
          logShipf("[BLE] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d inGar=%d strongOK=%d phase=%d",
                   r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
                   (int)gLeaveQual, (int)gInGarage, (int)gStrongOpenReady,
                   (int)phase);
        }

        switch (phase) {
          case BlePhase::WAIT_SIGNAL:
            if (hasSignal && gTrueNo) {
              gTrueNo = false;
              bool openOk = false;
              if (isStrong && openStrongPathAllowed()) {
                Serial.printf("[FSM] 真无→有-持续强 rssi=%d，发开码\n", r);
                openOk = autoOpenThenArm("真无→有-持续强", true);
              } else if (openWeakPathAllowed()) {
                Serial.printf("[FSM] 真无→有 rssi=%d，发开码\n", r);
                openOk = autoOpenThenArm("真无→有");
              } else {
                Serial.printf(
                    "[FSM] 开门拒绝（真无→有） rssi=%d inGar=%d suppress=%d strongOK=%d\n",
                    r, (int)gInGarage, (int)gDoor.autoOpenSuppressActive(),
                    (int)gStrongOpenReady);
              }
              phase = (openOk && isStrong) ? BlePhase::STRONG : BlePhase::APPEARING;
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
              if (openStrongPathAllowed()) {
                autoOpenThenArm("有→强-窗口达标", true);
              } else {
                Serial.printf(
                    "[FSM] 开门拒绝（有→强未达标） rssi=%d inGar=%d strongOK=%d hits=%d\n",
                    r, (int)gInGarage, (int)gStrongOpenReady, (int)gStrongStreak);
              }
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
            if (isStrong && openStrongPathAllowed()) {
              phase = BlePhase::STRONG;
              gLeaveQual = false;
              gRssiTrend.clear();
              Serial.println("[FSM] 弱→强持续，取消离开");
              break;
            }
            if (shouldCloseBySignal(hasSignal, isFar)) {
              Serial.println("[FSM] LEAVING 离场条件 → 发关码");
              tryCloseIfOpen("LEAVING离场");
              phase = BlePhase::WAIT_SIGNAL;
              break;
            }
            // 丢信号：门外安装由「强后丢失满时长」关门；弱路径开仍受库内锁
            if (!hasSignal) {
              phase = BlePhase::WAIT_SIGNAL;
              break;
            }
            // 弱信号保持 LEAVING，不再跳回 STRONG（否则 STRONG↔LEAVING 来回）
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

    if (gBtStackInited && gWeb.trackMode() == TRACK_MODE_CLASSIC &&
        gBt.ready() && gBt.autoTrack() && gBt.hasTarget()) {
      int r = gBt.lastRssi();
      // 经典纪元调度下 IDLE 探针 8s 一发 → 见面间隔最坏 ~10.6s，
      // 用 BLE 的 8s 阈值会周期性误判丢失（见 CLASSIC_SEEN_GAP_MS 注释）
      bool seen = gBt.seenRecently(CLASSIC_SEEN_GAP_MS);
      bool hasSignal = seen && r >= RSSI_APPEAR_MIN;
      bool isStrong = hasSignal && r >= RSSI_STRONG;
      bool isFar = false;
      bool lost = !seen;

      if (!clInited || seen != clPrevSeen || (seen && abs(r - clPrevRssi) >= 3) ||
          (!seen && clPrevSeen)) {
        observeSignal(hasSignal, hasSignal ? r : 0);
        isFar = debounceFar(hasSignal, hasSignal ? r : 0);
      }
      if (!seen) {
        observeSignal(false, 0);
        isFar = debounceFar(false, 0);
      }

      const bool closeDue = shouldCloseBySignal(hasSignal, isFar);

      clInited = true;
      clPrevSeen = seen;
      clPrevRssi = r;

      if (millis() - lastClLogMs >= 2000) {
        lastClLogMs = millis();
        Serial.printf(
            "[CLASSIC] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d inGar=%d strongOK=%d phase=%d\n",
            r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
            (int)gLeaveQual, (int)gInGarage, (int)gStrongOpenReady, (int)clPhase);
        // 关键变化进日志环（VPS 可回放开车离开的 RSSI 曲线）；稳态不刷环
        static int lastShipRssi = -999;
        static int lastShipSeen = -1;
        if ((int)seen != lastShipSeen ||
            (seen && abs(r - lastShipRssi) >= 5)) {
          lastShipSeen = (int)seen;
          lastShipRssi = seen ? r : -999;
          logShipf("[CLASSIC] rssi=%d strong=%d far=%d lost=%d trueNo=%d leaveQ=%d inGar=%d strongOK=%d phase=%d",
                   r, (int)isStrong, (int)isFar, (int)lost, (int)gTrueNo,
                   (int)gLeaveQual, (int)gInGarage, (int)gStrongOpenReady, (int)clPhase);
        }
      }

      switch (clPhase) {
        case ClPhase::WAIT_SIGNAL:
          if (hasSignal && gTrueNo) {
            gTrueNo = false;
            bool openOk = false;
            if (isStrong && openStrongPathAllowed()) {
              Serial.printf("[FSM] C 真无→有-窗口达标 rssi=%d，发开码\n", r);
              openOk = autoOpenThenArm("经典真无→有-窗口达标", true);
            } else if (openWeakPathAllowed()) {
              Serial.printf("[FSM] C 真无→有 rssi=%d，发开码\n", r);
              openOk = autoOpenThenArm("经典真无→有");
            } else {
              Serial.printf(
                  "[FSM] C 开门拒绝（真无→有） rssi=%d inGar=%d suppress=%d strongOK=%d\n",
                  r, (int)gInGarage, (int)gDoor.autoOpenSuppressActive(),
                  (int)gStrongOpenReady);
            }
            clPhase = (openOk && isStrong) ? ClPhase::STRONG : ClPhase::APPEARING;
            break;
          }
          if (closeDue) {
            Serial.println("[FSM] C WAIT 离场条件 → 发关码");
            tryCloseIfOpen("经典WAIT离场");
          }
          break;

        case ClPhase::APPEARING:
          if (isStrong) {
            if (openStrongPathAllowed()) {
              autoOpenThenArm("经典有→强-窗口达标", true);
              clPhase = ClPhase::STRONG;
            } else {
              // 未达标：不进 STRONG，继续观察窗口累计
              Serial.printf(
                  "[FSM] C 开门拒绝（有→强未达标） rssi=%d inGar=%d strongOK=%d hits=%d\n",
                  r, (int)gInGarage, (int)gStrongOpenReady, (int)gStrongStreak);
            }
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
          if (isStrong && openStrongPathAllowed()) {
            clPhase = ClPhase::STRONG;
            gLeaveQual = false;
            gRssiTrend.clear();
            Serial.println("[FSM] C 弱→强持续，取消离开");
            break;
          }
          if (closeDue) {
            Serial.println("[FSM] C LEAVING 离场条件 → 发关码");
            tryCloseIfOpen("经典LEAVING离场");
            clPhase = ClPhase::WAIT_SIGNAL;
            break;
          }
          if (!hasSignal) {
            clPhase = ClPhase::WAIT_SIGNAL;
            break;
          }
          break;
      }
    }
  }

  // 时钟首次同步留证据行：远程凭这条确认「事件时间戳」已生效
  {
    static bool timeSyncLogged = false;
    if (!timeSyncLogged) {
      time_t tn = time(nullptr);
      if (tn > 1600000000) {
        timeSyncLogged = true;
        struct tm tmv;
        char b[24] = "?";
        if (localtime_r(&tn, &tmv)) strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
        logShipf("[TIME] NTP synced (event ts on) now=%s", b);
      }
    }
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 8000) {
    lastLog = millis();
    // 心跳恒发：logShipf 内部已把"环推送"和"串口写出"分开——环必达 VPS，
    // 串口只在 TX FIFO 有余量时打（>96 < FIFO 深度 128）。
    // 旧写法在 logShipf 外面再包一层 availableForWrite>96：无串口主机的设备
    // FIFO 恒满 → 心跳整段丢失（1388 8 小时 0 条 [LOG] 实证），断网期
    // sta/rssi 面包屑全丢。现在只让串口那一半承担门控。
    // rssi 段按跟踪模式取信号源：经典→gBt；BLE→gBleScan（否则 BLE 模式下
    // 心跳永远显示经典路径的旧值/-127）。
    String rssiSeg;
    if (trackMode == TRACK_MODE_BLE) {
      char b[96];
      int r = gBleScan.matchRssi();
      bool seen = gBleScan.lastMatchMs() != 0 &&
                  (millis() - gBleScan.lastMatchMs()) < BLE_SILENT_GAP_MS;
      snprintf(b, sizeof(b), "rssi=%d raw=%d seen=%d label=%s", r, r,
               seen ? 1 : 0,
               gBleScan.matchLabel().length() ? gBleScan.matchLabel().c_str()
                                              : "-");
      rssiSeg = b;
    } else {
      rssiSeg = gBt.debugLine();
    }
    logShipf(
        "[LOG] heap=%u maxblk=%u sta=%d http=%s | %s | %s | %s",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (int)gWeb.staConnected(),
        gWeb.staConnected() ? "up" : "down",
        gDoor.debugLine().c_str(), rssiSeg.c_str(),
        gNfc.debugLine().c_str());
  }
}
