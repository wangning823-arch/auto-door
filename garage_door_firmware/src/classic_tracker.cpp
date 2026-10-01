#include "classic_tracker.h"
#include "config.h"
#include "http_client.h"
#include "log_ship.h"
#include "remote_ota.h"
#include "BluetoothSerial.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_heap_caps.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Classic Bluetooth not enabled"
#endif

// 自动 Inquiry 回调丢失时的强清超时（len≈2 → 约 2.6s；留足余量）
static const uint32_t INQUIRY_STUCK_MS = 8000;
// 方向A：inquiry 结束后再静默一小段，给 BTU 异步 malloc(4112) 留连续块
// 注意：inquiry 周期 4000ms、len=2≈2560ms，quiet 必须 < 4000-2560，
// 否则 HTTP/status/log 窗口被吃光（1388 OTA 0.2.202610010712 实锤：
// quiet=1500 后 20min 零日志；恢复 800ms 才有空窗）。
static const uint32_t POST_INQUIRY_QUIET_MS = 800;

static BluetoothSerial SerialBT;
static ClassicTracker* gTracker = nullptr;
static bool gBtReady = false;

volatile uint32_t ClassicTracker::s_inqCount = 0;
volatile uint32_t ClassicTracker::s_thinCount = 0;
volatile uint32_t ClassicTracker::s_btuFailCount = 0;
volatile bool ClassicTracker::s_btuResGiveReq = false;

static uint32_t btLargest8() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

// ===== BTU 专用应急堆（20260930 dda0；20261001 尺寸自适应）=====
// 根因：BT air / OTA 气囊在 inquiry 前一次性给完，autoTrack 下不再 rearm；
// dda0 碎片期 max8 常 2292~4084，BTU 4112 必挂且 thin≈btufail。
// 应急块与 OTA 气囊分离：只在 largest8<4112（或 BTU 失败）时释放。
// 20261001 实测：固定 8192 在 dda0 稳态（maxblk 2292~7156）永远抓不到
// → 自适应降级：8192 失败试 6144，再失败试 4608（仍 ≥ BTU 的 4112）。
// 每档都要求 largest ≥ sz+2308（给 WiFi 2308 留位置），抓到哪档算哪档。
static const uint32_t kBtuReserveSizes[] = {8192, 6144, 4608};
static const uint32_t kBtuReserveWifiGap = 2308;
static void* s_btuReserve = nullptr;
static uint32_t s_btuReserveSz = 0;
// 首持推迟：begin() 只置 armed，真正 force hold 在 serviceBtuReserve（loop 阶段，
// 且等 STA 连上或开机 30s）——setup 里 startStaFromStore→WiFi.begin 之前堆里
// 绝不能多钉大块。20261001 dda0 实锤：1246 在 begin() 就 hold → WiFi.begin 时
// max8=40948 差一口气 → wifi:ieee80211_ioctl.c 1612 → wifi task WDT 死循环；
// 1388 同镜像 max8=45044 侥幸过。同镜像同配置，纯碎片阈值差异。
static bool s_btuResArmed = false;
static uint32_t s_btuResNextTryMs = 0;

void ClassicTracker::noteAllocFail(size_t size, const char* task) {
  // 钩子内：只计数+置位，严禁分配/printf/free
  // BTU inquiry 要 4112；任务名含 BTU 也算
  if (size == 4112 || (task && (strstr(task, "BTU") || strstr(task, "btu")))) {
    s_btuFailCount++;
    s_btuResGiveReq = true;  // loop 里释放应急堆，供下一轮 inquiry
  }
}

bool ClassicTracker::btuReserveHeld() { return s_btuReserve != nullptr; }

static void btuReserveDrop(const char* why) {
  if (!s_btuReserve) return;
  free(s_btuReserve);
  s_btuReserve = nullptr;
  s_btuReserveSz = 0;
  logShipf("[HEAP] BTU reserve dropped why=%s max8=%u", why,
           (unsigned)btLargest8());
}

// 自适应尺寸 hold：从大到小试，每档要求 largest ≥ sz+2308（给 WiFi 2308 留位），
// 抓到哪档算哪档。drop 后 4608 空间即可盖住 BTU 4112。
static bool btuReserveTryHoldAdaptive() {
  if (s_btuReserve) return true;
  const uint32_t largest = btLargest8();
  for (uint32_t i = 0; i < sizeof(kBtuReserveSizes) / sizeof(kBtuReserveSizes[0]);
       i++) {
    const uint32_t sz = kBtuReserveSizes[i];
    if (largest < sz + kBtuReserveWifiGap) continue;
    s_btuReserve = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (s_btuReserve) {
      s_btuReserveSz = sz;
      logShipf("[HEAP] BTU reserve held sz=%u max8=%u", (unsigned)sz,
               (unsigned)btLargest8());
      return true;
    }
  }
  return false;
}

void ClassicTracker::serviceBtuReserve(bool staUp) {
  if (s_btuResGiveReq) {
    s_btuResGiveReq = false;
    btuReserveDrop("btu_fail");
    return;
  }
  // hold 窗口重试（非一次性）：首持条件满足后每 5s 试一次自适应 hold。
  // drop（inquiry 前腾块/btu_fail）后靠这里抓回来，不再依赖 ≥12288 才 rearm
  // ——1656 实测 dda0 稳态永远到不了 12288，reserve 一次 drop 就永久消失。
  // staUp 由 main 传入（本模块不依赖 WiFi：STA 已连 或 开机 30s 后才 hold，
  // 保证 WiFi.begin 已安全返回；无 WiFi 的部署 30s 后照常 hold）。
  if (!s_btuResArmed || s_btuReserve) return;
  if (!staUp && millis() < 30000UL) return;
  const uint32_t now = millis();
  if (now < s_btuResNextTryMs) return;
  s_btuResNextTryMs = now + 5000UL;
  btuReserveTryHoldAdaptive();
}

// ===== Inquiry 同步 BT 气囊（不降低 inquiry 频率）=====
// 根因：片内稳态最大连续块常 2804~4084，BTU inquiry 要 4112 → 总差一口气。
// 1243「开机 hold 12KB」把外面挤到 1908，更糟；1435 停 hold 后又没有可归还块。
// 1844 方案：单独一块，**BT 栈起来后立刻抢一块**（此时堆还干净），
// **启动 inquiry 前一定 free**，**结束后尽量 re-hold**。周期仍约 3s，不降频。
// dda0 实测：稳态 maxblk 常 4084，若 hold 门槛要求 largest8≥10KB 则永远占不到
// → 必须在 begin() 时 force malloc，而不是等池子“宽裕”。
static const uint32_t kBtAirSize = 8192;
static const uint32_t kBtAirSizeMin = 4608;  // 至少盖住 BTU 4112
static void* s_btAir = nullptr;

static void btAirTryHoldForce() {
  if (s_btAir) return;
  s_btAir = malloc(kBtAirSize);
  uint32_t sz = kBtAirSize;
  if (!s_btAir) {
    s_btAir = malloc(kBtAirSizeMin);
    sz = kBtAirSizeMin;
  }
  if (s_btAir) {
    logShipf("[HEAP] BT air held force sz=%u max8=%u", (unsigned)sz,
             (unsigned)btLargest8());
  }
}

static void btAirTryHold() {
  if (s_btAir) return;
  const uint32_t largest = btLargest8();
  // 1913：drop 后门槛过低会立刻 re-hold，BTU 只剩 4084。
  // 2206 实测：largest≥8000 就收 4608 会把最大块切到 4084（4608+4112>8000），
  // inquiry 前永远 thin，BTU 4112 必挂。
  // 20261001 dda0：门槛 10000 太高（稳态 maxblk 7156，气囊 drop 后永远抓不回，
  // inquiry 前无块可腾 → btufail 40/10min）。修正为：
  //   收 4608 须 largest ≥ 6916（4608+2308：切完还剩 WiFi 的位置；
  //   inquiry 前 drop 时相邻空闲合并回 ≥4608 > BTU 4112）
  //   收 8192 仍 ≥12288。仅 inquiry 空闲时调用；启动前一律 drop。
  if (largest >= 12288) {
    s_btAir = malloc(kBtAirSize);
    if (s_btAir) {
      logShipf("[HEAP] BT air held max8=%u", (unsigned)btLargest8());
      return;
    }
  }
  if (largest < 4608 + kBtuReserveWifiGap) return;
  s_btAir = malloc(kBtAirSizeMin);
  if (s_btAir) {
    logShipf("[HEAP] BT air held min max8=%u", (unsigned)btLargest8());
  }
}

static void btAirDropForInquiry() {
  if (s_btAir) {
    free(s_btAir);
    s_btAir = nullptr;
    logShipf("[HEAP] BT air drop for inquiry max8=%u", (unsigned)btLargest8());
  }
}

// 方向A：inquiry 前尽量腾出 ≥4112 连续块给 BTU
// OTA 气囊只要占着就先归还；BT air drop；
// 仍 <4112 时释放 BTU 专用应急块（与 OTA 气囊分离，避免一次给完就没）；
// free 后仍薄则 noteThin，但不推迟 inquiry。
static void ensureBtuHeapForInquiry() {
  if (remoteOtaReserveHeld()) {
    remoteOtaReserveGive();
  }
  btAirDropForInquiry();
  uint32_t l = btLargest8();
  if (l < 4112) {
    if (s_btuReserve) {
      btuReserveDrop("thin_before_inq");
      l = btLargest8();
    }
  }
  if (l < 4112) {
    ClassicTracker::noteThin();
    // 精确统计：thin 次数 + 当前 BTU 累计失败 + 总 fail，不依赖 HEAPFAIL 抽样
    logShipf("[HEAP] BT thin before inq max8=%u thin=%u btufail=%u",
             (unsigned)l, (unsigned)ClassicTracker::thinCount(),
             (unsigned)ClassicTracker::btuFailCount());
  }
}

bool ClassicTracker::btQuietForHttp() const {
  if (inquiryBusy_ || discRunning_) return true;
  return postQuietUntilMs_ != 0 && millisBefore(millis(), postQuietUntilMs_);
}

static String macToStr(const uint8_t* bda) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1],
           bda[2], bda[3], bda[4], bda[5]);
  return String(buf);
}

static String codToKind(uint32_t cod) {
  uint32_t major = (cod >> 8) & 0x1F;
  switch (major) {
    case 0x01: return "电脑";
    case 0x02: return "手机";
    case 0x03: return "网络";
    case 0x04: return "音频";
    case 0x05: return "外设";
    case 0x06: return "影像";
    case 0x07: return "穿戴";
    default: return "";
  }
}

// 从 EIR 完整数据里抠本地名称
static bool nameFromEir(const uint8_t* eir, int eirLen, String& out) {
  if (!eir || eirLen <= 0) return false;
  int i = 0;
  while (i + 1 < eirLen) {
    uint8_t len = eir[i];
    if (len == 0) break;
    if (i + len >= eirLen) break;
    uint8_t type = eir[i + 1];
    if (type == 0x09 || type == 0x08) {  // Complete / Shortened Local Name
      int nlen = len - 1;
      if (nlen > 0) {
        char tmp[248] = {0};
        if (nlen > 247) nlen = 247;
        memcpy(tmp, &eir[i + 2], nlen);
        out = String(tmp);
        return out.length() > 0;
      }
    }
    i += len + 1;
  }
  return false;
}

static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (!gTracker) return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      String mac = macToStr(param->disc_res.bda);
      int rssi = -90;
      String name;
      uint32_t cod = 0;
      int nprop = param->disc_res.num_prop;
      for (int i = 0; i < nprop; i++) {
        esp_bt_gap_dev_prop_t* p = &param->disc_res.prop[i];
        if (!p || !p->val) continue;
        if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->len >= 1) {
          rssi = *(int8_t*)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
          name = String((const char*)p->val);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_COD && p->len >= 4) {
          cod = *(uint32_t*)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->len > 0) {
          String n2;
          if (nameFromEir((const uint8_t*)p->val, p->len, n2) && name.length() == 0) {
            name = n2;
          }
        }
      }
      if (name.length()) {
        gTracker->onClassicDevice(mac, rssi, name);
      } else {
        // 没带名称：先登记，再异步读远程名称
        gTracker->onClassicDevice(mac, rssi, "");
        uint8_t bda[6];
        for (int i = 0; i < 6; i++) {
          unsigned v = 0;
          sscanf(mac.c_str() + i * 3, "%02x", &v);
          bda[i] = (uint8_t)v;
        }
        esp_bt_gap_read_remote_name(bda);
      }
      break;
    }
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        String mac = macToStr(param->read_rmt_name.bda);
        String name = String((const char*)param->read_rmt_name.rmt_name);
        if (name.length()) {
          gTracker->onDeviceName(mac, name);
        }
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        gTracker->onInquiryDone();
      }
      break;
    }
    default:
      break;
  }
}

bool ClassicTracker::begin(const char* macStr) {
  gTracker = this;
  targetMac_ = String(macStr);
  targetMac_.toUpperCase();
  targetSet_ = (targetMac_.length() == 17);

  if (!gBtReady) {
    if (!SerialBT.begin("GarageDoor")) {
      Serial.println("[BT] SerialBT.begin FAILED");
      ready_ = false;
      return false;
    }
    // 默认不可被搜索/不可连：避免关配对后仍被手机搜到 GarageDoor
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_gap_register_callback(gapCallback);
    gBtReady = true;
    Serial.println("[BT] Classic ready, NON_DISCOVERABLE (仅按需 inquiry)");
  }
  ready_ = true;

  // BT 栈起来后立刻抢 BT 气囊：此时堆还干净，dda0 后期 maxblk 常只剩 4084，
  // 等“宽裕再 hold”会永远 hold 不上。
  btAirTryHoldForce();
  // BTU 应急块：只置 armed，真正 hold 推迟到 STA 连上后（serviceBtuReserve）。
  // setup 期 WiFi.begin 前多钉 8192 会把 dda0 推过 wifi WDT 阈值（见常量处注释）。
  s_btuResArmed = true;

  Serial.printf("[BT] target MAC %s -> %s\n", macStr, targetSet_ ? "OK" : "INVALID");
  nextInquiryMs_ = millis() + 1000;
  return true;
}

void ClassicTracker::setInquiryPaused(bool paused) {
  inquiryPaused_ = paused;
  if (paused) cancelActiveInquiry();
}

void ClassicTracker::setInquirySlow(bool slow) {
  inquirySlow_ = slow;
  if (slow) {
    // 让出射频给 SoftAP，但仍保留跟踪
    uint32_t want = millis() + 8000;
    if (millisBefore(nextInquiryMs_, want)) nextInquiryMs_ = want;
  }
}

void ClassicTracker::cancelActiveInquiry() {
  if (!gBtReady) return;
  if (inquiryBusy_ && !discRunning_) {
    esp_bt_gap_cancel_discovery();
    // 回调可能不回：记起点，由 loop 超时强清
    if (inquiryStartMs_ == 0) inquiryStartMs_ = millis();
  }
}

int ClassicTracker::lastRssi() const {
  // 旧值会误导网页/状态：太久没扫到就当作丢失
  if (lastSeenMs_ != 0 && (millis() - lastSeenMs_) > 20000) return -127;
  return lastRssi_;
}

void ClassicTracker::startDiscovery(uint32_t durationMs) {
  if (!gBtReady) {
    Serial.println("[BT] startDiscovery: BT not ready");
    return;
  }
  // 用户主动扫描：先取消可能卡住的 inquiry
  esp_bt_gap_cancel_discovery();
  inquiryBusy_ = false;
  delay(50);

  discList_.clear();
  discRunning_ = true;
  discEndMs_ = millis() + durationMs;
  ensureBtuHeapForInquiry();
  inquiryBusy_ = true;
  inquiryStartMs_ = millis();
  // length 单位 1.28s，0x01–0x30；用 8 ≈ 10s
  uint8_t len = (uint8_t)constrain((durationMs + 1279) / 1280, 2, 48);
  esp_err_t err =
      esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, len, 0);
  Serial.printf("[BT] inquiry start len=%u ret=%d (%s)\n", len, (int)err,
                err == ESP_OK ? "OK" : esp_err_to_name(err));
  if (err != ESP_OK) {
    inquiryBusy_ = false;
    inquiryStartMs_ = 0;
    discRunning_ = false;
  }
}

bool ClassicTracker::discoveryRunning() const {
  return discRunning_ && !millisReached(millis(), discEndMs_ + 1);
}

void ClassicTracker::onClassicDevice(const String& mac, int rssi, const String& name) {
  String m = mac;
  m.toUpperCase();
  Serial.printf("[BT] FOUND %s rssi=%d name=%s\n", m.c_str(), rssi,
                name.length() ? name.c_str() : "(none)");

  if (discRunning_ && !millisReached(millis(), discEndMs_ + 1)) {
    bool found = false;
    for (auto& it : discList_) {
      if (it.mac == m) {
        if (rssi > it.rssi) it.rssi = rssi;
        if (it.name.length() == 0 && name.length()) it.name = name;
        found = true;
        break;
      }
    }
    if (!found && discList_.size() < 32) {
      discList_.push_back({m, rssi, name});
    }
  }

  if (targetSet_ && m == targetMac_) {
    if (rssi == 0) rssi = -70;
    lastRssi_ = rssi;
    missCount_ = 0;  // 扫到了，清零漏扫
    pushSample(true, rssi);
    recordTs((int16_t)rssi);
    computeSlope();
    classifyTrend(true, rssi);
    updateZone();
  }
}

void ClassicTracker::onDeviceName(const String& mac, const String& name) {
  String m = mac;
  m.toUpperCase();
  for (auto& it : discList_) {
    if (it.mac == m) {
      if (it.name.length() == 0) it.name = name;
      Serial.printf("[BT] name %s = %s\n", m.c_str(), name.c_str());
      return;
    }
  }
  // 名称先到、列表还没插入时也补一条
  if (discRunning_ && discList_.size() < 32) {
    discList_.push_back({m, -90, name});
  }
}

void ClassicTracker::onInquiryDone() {
  inquiryBusy_ = false;
  inquiryStartMs_ = 0;
  postQuietUntilMs_ = millis() + POST_INQUIRY_QUIET_MS;
  Serial.printf("[BT] inquiry stopped, list=%u miss=%u rssi=%d\n",
                (unsigned)discList_.size(), missCount_, lastRssi_);
  // 漏扫不清零：连续 3 轮未见才算不可见
  if (targetSet_ && !discRunning_) {
    if (missCount_ < 255) missCount_++;
    if (missCount_ >= 3) {
      pushSample(false, -127);
      recordTs(-127);
      computeSlope();
      classifyTrend(false, lastRssi_);
      updateZone();
    }
  }
  // 本轮 inquiry 结束后立刻尝试收回 BT 气囊（不降频，只为下一轮准备连续块）
  btAirTryHold();
  // BTU 应急堆：inquiry 结束是最佳回收时机（刚 drop 过、碎片窗口最宽），
  // 自适应尺寸直接试抓；service 侧还有 5s 兜底重试
  if (s_btuResArmed && !s_btuReserve) btuReserveTryHoldAdaptive();
  if (discRunning_ && !millisReached(millis(), discEndMs_ + 1) && !inquiryPaused_) {
    ensureBtuHeapForInquiry();
    inquiryBusy_ = true;
    inquiryStartMs_ = millis();
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 3, 0);
  }
}

std::vector<ClassicDeviceItem> ClassicTracker::discoveryResults() const { return discList_; }

void ClassicTracker::pushSample(bool visible, int rssi) {
  hist_[histHead_] = visible ? (int8_t)constrain(rssi, -127, 0) : (int8_t)-127;
  histHead_ = (histHead_ + 1) % WIN;
  if (histCount_ < WIN) histCount_++;
}

void ClassicTracker::recordTs(int16_t rssi) {
  tsMs_[tsHead_] = millis();
  tsRssi_[tsHead_] = rssi;
  tsHead_ = (uint16_t)((tsHead_ + 1) % TS_N);
  if (tsCount_ < TS_N) tsCount_++;
}

void ClassicTracker::clearTs() {
  tsHead_ = 0;
  tsCount_ = 0;
}

int ClassicTracker::tsExport(uint32_t* tSec, int16_t* rssi, int maxN) const {
  if (!tSec || !rssi || maxN <= 0 || tsCount_ == 0) return 0;
  int n = tsCount_;
  if (n > maxN) n = maxN;
  int start = (tsHead_ - n + TS_N * 2) % TS_N;
  uint32_t base = tsMs_[start];
  for (int i = 0; i < n; i++) {
    int idx = (start + i) % TS_N;
    uint32_t ms = tsMs_[idx];
    tSec[i] = (ms >= base) ? ((ms - base) / 1000u) : 0;
    rssi[i] = tsRssi_[idx];
  }
  return n;
}

void ClassicTracker::computeSlope() {
  if (histCount_ < 3) {
    slope_ = 0;
    return;
  }
  float sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
  int n = 0;
  for (int i = 0; i < histCount_; i++) {
    int idx = (histHead_ - histCount_ + i + WIN * 2) % WIN;
    if (hist_[idx] <= -127) continue;
    float x = (float)n;
    float y = (float)hist_[idx];
    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumXX += x * x;
    n++;
  }
  if (n < 3) {
    slope_ = 0;
    return;
  }
  float denom = n * sumXX - sumX * sumX;
  if (fabsf(denom) < 1e-6f) {
    slope_ = 0;
    return;
  }
  slope_ = (n * sumXY - sumX * sumY) / denom;
}

void ClassicTracker::classifyTrend(bool visible, int rssi) {
  uint32_t now = millis();
  if (visible) {
    if (!wasVisible_ && (lastSeenMs_ == 0 || (now - lastSeenMs_) > T_SILENT_GAP_MS)) {
      if (lastRssi_ <= -127 && rssi >= RSSI_OPEN) {
        trend_ = SignalTrend::SUDDEN_APPEAR;
      }
    }
    lastSeenMs_ = now;
    silentSinceMs_ = 0;
    wasVisible_ = true;
    if (trend_ != SignalTrend::SUDDEN_APPEAR) {
      if (slope_ >= SLOPE_MIN && lastRssi_ >= RSSI_OPEN) {
        trend_ = SignalTrend::GRADUAL_IN;
      } else if (slope_ <= -SLOPE_MIN && lastRssi_ <= RSSI_FADE) {
        trend_ = SignalTrend::GRADUAL_OUT;
      } else {
        trend_ = SignalTrend::STEADY;
      }
    }
  } else {
    if (wasVisible_) {
      if (lastRssi_ >= RSSI_FADE && slope_ > -SLOPE_MIN) {
        trend_ = SignalTrend::SUDDEN_LOSS;
      } else if (slope_ <= -SLOPE_MIN || lastRssi_ <= RSSI_FADE) {
        trend_ = SignalTrend::GRADUAL_OUT;
      }
      silentSinceMs_ = now;
      wasVisible_ = false;
    } else if (silentSinceMs_ != 0) {
      if (trend_ == SignalTrend::SUDDEN_LOSS && (now - silentSinceMs_) > T_CLEAR_MS) {
        trend_ = SignalTrend::UNKNOWN;
      }
    }
  }
}

void ClassicTracker::updateZone() {
  bool clearOk = silentSinceMs_ != 0 && (millis() - silentSinceMs_) >= T_CLEAR_MS;
  if (zone_ == CarZone::TRANSIT) {
    if (clearOk && trend_ == SignalTrend::GRADUAL_OUT) zone_ = CarZone::OUT;
    return;
  }
  if (lastRssi_ >= RSSI_OPEN &&
      (trend_ == SignalTrend::GRADUAL_IN || trend_ == SignalTrend::STEADY)) {
    zone_ = CarZone::IN_GARAGE;
    everInGarage_ = true;
  } else if (wasVisible_ || (lastSeenMs_ && (millis() - lastSeenMs_) < 5000)) {
    zone_ = CarZone::NEAR;
    if (trend_ == SignalTrend::GRADUAL_OUT || trend_ == SignalTrend::SUDDEN_LOSS) {
      zone_ = CarZone::TRANSIT;
    }
  } else if (clearOk) {
    zone_ = CarZone::OUT;
  } else if (trend_ == SignalTrend::SUDDEN_LOSS) {
    zone_ = CarZone::TRANSIT;
  } else {
    zone_ = CarZone::OUT;
  }
}

void ClassicTracker::loop() {
  const uint32_t now = millis();

  if (discRunning_ && millisReached(now, discEndMs_ + 1)) {
    discRunning_ = false;
    inquiryBusy_ = false;
    inquiryStartMs_ = 0;
    Serial.printf("[BT] discovery done, %u devices\n", (unsigned)discList_.size());
  }

  // Inquiry 回调丢失兜底：busy 超时强清，否则经典跟踪永久卡死
  if (inquiryBusy_ && !discRunning_ && inquiryStartMs_ != 0 &&
      (now - inquiryStartMs_) >= INQUIRY_STUCK_MS) {
    Serial.printf("[BT] inquiry stuck >%ums → cancel + clear busy\n",
                  (unsigned)INQUIRY_STUCK_MS);
    if (gBtReady) esp_bt_gap_cancel_discovery();
    inquiryBusy_ = false;
    inquiryStartMs_ = 0;
  }

  // 显式暂停时才停后台跟踪；SoftAP 慢速模式仍要扫（否则手机连热点时车走了永远不关）
  if (!autoTrack_ || inquiryPaused_ || !gBtReady || !targetSet_ || discRunning_ ||
      inquiryBusy_) {
    // 跟踪空闲时尽量把 BT 气囊占住，供下一轮 inquiry 归还
    if (autoTrack_ && gBtReady && targetSet_ && !inquiryBusy_ && !discRunning_) {
      btAirTryHold();
    }
    return;
  }
  if (millisReached(now, nextInquiryMs_)) {
    // Inquiry 优先：4s 节奏（约 2.5s 占用 + 1.5s 空窗给 HTTP），不被 HTTP 推迟。
    // HTTP 在空窗发送，发不完就放弃。
    ensureBtuHeapForInquiry();
    ClassicTracker::noteInquiryStart();
    inquiryBusy_ = true;
    inquiryStartMs_ = now;
    uint32_t gap = inquirySlow_ ? 15000 : 4000;
    nextInquiryMs_ = now + gap;
    uint8_t len = inquirySlow_ ? 1 : 2;  // 1≈1.28s，短一些少打网页
    esp_err_t err =
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, len, 0);
    Serial.printf("[BT] auto inquiry slow=%d ret=%d\n", (int)inquirySlow_,
                  (int)err);
    if (err != ESP_OK) {
      inquiryBusy_ = false;
      inquiryStartMs_ = 0;
      btAirTryHold();
    }
  }
}

bool ClassicTracker::seenRecently(uint32_t withinMs) const {
  return lastSeenMs_ != 0 && (millis() - lastSeenMs_) <= withinMs;
}

void ClassicTracker::markLeftForCloseEval() {}

String ClassicTracker::debugLine() const {
  char buf[180];
  snprintf(buf, sizeof(buf),
           "rssi=%d raw=%d slope=%.2f trend=%d zone=%d seen=%lu auto=%d slow=%d",
           lastRssi(), lastRssiRaw(), (double)slope_, (int)trend_, (int)zone_,
           (unsigned long)lastSeenMs_, (int)autoTrack_, (int)inquirySlow_);
  return String(buf);
}

bool btRadioPowerDown() {
  SerialBT.end();  // bluedroid disable+deinit（_stop_bt）
  delay(50);
  bool ok = btStop();  // controller disable+deinit → IDLE
  Serial.printf("[BT] radio power down: %s\n", ok ? "OK" : "FAIL");
  return ok;
}
