#pragma once

// ===== 车库门控制器 P0 配置 =====
// 引脚按 ESP32 DevKit / WROOM32 常见接法，可按实际改

// 固件版本：由 tools/bump_version.py 编译前生成（0.2.源码时间戳）
#include "fw_version.h"

// 继电器（干接点脉冲，模拟墙控/遥控按键）
// 继电器控制脚：默认 G21（右列，避开紧挨的 GND）
#ifndef PIN_RELAY
#define PIN_RELAY 21
#endif
// 五步对照实测：低=吸合亮，高=释放灭 → 低电平触发
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif

// 门磁输入：已停用（开/关为不同 RF 码，不靠门磁判门态）
// #define PIN_DOOR_MAGNET 27

// 米家插座路径：TRIG 干接点输入（上拉，短接到 GND=触发）
#ifndef PIN_TRIG_IN
#define PIN_TRIG_IN 25
#endif

// ===== 远程令：VPS 轮询（蓝牙空隙才访问 WiFi）=====
// 改成你的 VPS：仅 443 对外 → https://door.wzx.homes/dev/poll
#ifndef REMOTE_POLL_URL
#define REMOTE_POLL_URL "https://door.wzx.homes/dev/poll"
#endif
// HTTPS MVP：跳过证书校验（后续可换成 CA）；0 则要求证书有效
#ifndef REMOTE_HTTP_INSECURE
#define REMOTE_HTTP_INSECURE 1
#endif
// TLS 握手需要时间；过短会导致连不上 nginx
#ifndef REMOTE_POLL_TIMEOUT_MS
#define REMOTE_POLL_TIMEOUT_MS 5000
#endif
// 轮询周期须明显小于服务端 TTL（8s），留出蓝牙空隙
#ifndef REMOTE_POLL_INTERVAL_MS
#define REMOTE_POLL_INTERVAL_MS 3000
#endif
#ifndef REMOTE_CMD_ENABLE_DEFAULT
#define REMOTE_CMD_ENABLE_DEFAULT 0
#endif
#ifndef REMOTE_BT_GAP_MIN_MS
#define REMOTE_BT_GAP_MIN_MS 100
#endif

// 学习/配对实体键（上拉，按下=低）
#ifndef PIN_LEARN_BTN
#define PIN_LEARN_BTN 0
#endif

// 状态 LED（部分板载 IO2）
#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 2
#endif

// 433MHz 接收模块 DATA（超再生/超外差模块）
#ifndef PIN_RF_DATA
#define PIN_RF_DATA 13
#endif

// 433MHz 发射模块 DATA（学习回放用）
#ifndef PIN_RF_TX
#define PIN_RF_TX 26
#endif

// ===== RF 发射安全（防整夜发射堵死门机遥控）=====
// rfauto 仅允许短时联调；到时自动 OFF 并写回 NVS
#ifndef RF_AUTO_MAX_MS
#define RF_AUTO_MAX_MS 120000UL  // 2 分钟
#endif
// 空闲时 TX 被拉高超过此时长 → 视为卡死，强制拉低
#ifndef RF_TX_STUCK_MS
#define RF_TX_STUCK_MS 80UL
#endif
// 上电不从 NVS 恢复 rfauto（旧固件曾因工具自动 rfauto on 整夜发射）
#ifndef RF_AUTO_RESTORE_ON_BOOT
#define RF_AUTO_RESTORE_ON_BOOT 0
#endif

// ===== 固定码多键学习回放（315/433 遥控）=====
#define RF_KEY_COUNT         4       // 0=开/上 1=关/下 2=暂停 3=锁定
#define RF_KEY_MAX_PULSES    80      // 单帧上限（一帧约50）
#define RF_PLAY_REPEATS      12      // 回放重复帧数（门机常要按久一点）
#define RF_FRAME_GAP_US      5000    // 帧间隔
#define RF_INTER_FRAME_MIN_US 4000   // ≥此值视为帧间隔，存单帧时丢弃

// 学习槽语义（串口别名）
#define RF_KEY_OPEN          0       // 上/开
#define RF_KEY_CLOSE         1       // 下/关
#define RF_KEY_STOP          2       // 暂停
#define RF_KEY_LOCK          3       // 锁定

// PN532 NFC（I2C 模式，模块焊盘拨到 I2C）
// GPIO21 已被继电器占用，I2C 改用 16/17
#ifndef PIN_NFC_SDA
#define PIN_NFC_SDA 16
#endif
#ifndef PIN_NFC_SCL
#define PIN_NFC_SCL 17
#endif

// RF 抓包参数
#define RF_CAPTURE_MAX_PULSES  512     // 最大记录脉冲数
#define RF_CAPTURE_GAP_US      8000    // 静默 >8ms 视为一帧间隔
#define RF_CAPTURE_TIMEOUT_MS  8000    // 抓包超时 8 秒

// ===== 目标车机蓝牙 MAC（默认值；运行时可由网页/NVS 覆盖）=====
#ifndef CAR_BT_MAC
#define CAR_BT_MAC "58:C4:1E:84:59:8B"
#endif

// ===== 配置热点（手机连上后浏览器设置）=====
// 信道 1：与多数路由器错开时更稳；6 也可
#ifndef AP_SSID_PREFIX
#define AP_SSID_PREFIX "GarageDoor-"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "12345678"
#endif
#ifndef AP_CHANNEL
#define AP_CHANNEL 1
#endif
#ifndef AP_MAX_CONN
#define AP_MAX_CONN 4
#endif
// WiFi 调试模式：1=每次上电强制开 SoftAP；0=尊重 NVS wifi_on
// 关热点后若复位，1 会把热点拉回来 → 自动门用 0
#ifndef WIFI_DEBUG_BOOT_ON
#define WIFI_DEBUG_BOOT_ON 0
#endif
// SoftAP 打开期间蓝牙让射频：手机才能稳定关联并打开网页
// （关联完成前 stationNum 仍可能为 0，不能只在「有客户端」时才降级）
#ifndef WIFI_AP_YIELD_BT
#define WIFI_AP_YIELD_BT 1
#endif
// SoftAP 启动后射频静默窗口：暂停经典 Inquiry / BLE 扫描，优先让热点+HTTP 稳定
#ifndef WIFI_AP_BOOT_QUIET_MS
#define WIFI_AP_BOOT_QUIET_MS 45000
#endif
// SoftAP 调试：经典BT/BLE 栈推迟初始化（网页优先）
// 无客户端时至少等到 WIFI_AP_BT_START_MS；有客户端则继续等，最长 WIFI_AP_BT_START_MAX_MS
#ifndef WIFI_AP_BT_START_MS
#define WIFI_AP_BT_START_MS 12000
#endif
#ifndef WIFI_AP_BT_START_MAX_MS
#define WIFI_AP_BT_START_MAX_MS 90000
#endif

// ===== 信号与安全参数 =====
#define RSSI_SAMPLE_MS        1000
#define RSSI_WINDOW           10      // 滑动窗口点数
#define RELAY_PULSE_MS        2000    // 遥控按压时长；1s 电机常不认，改 2s

// ===== BLE 状态机 =====
// 开：无→有 且 rssi < -80 → 立刻开（关着门贴近约 -90 也能开，不等渐强）
// 例外：无→有 且 rssi ≥ -80 → 不开（库内开关蓝牙等突变，一次就很“强”）
// 关：进库变强后离开；RSSI≤RSSI_FAR_CLOSE 连续 N 次 ≈ 走出约10m 即关
// 实测参考（ESP32 在库内靠门侧）：
//   车在库外远处 ~-97；库外门口 ~-75~-93（关门）；库内强 ~-70 以上
#define RSSI_APPEAR_MIN       -110    // 出现信号下限（≥此值算「有」）
#define RSSI_STRONG           -80     // 强信号阈值（离场路径用）
// 首见就 ≥ 此值：视为库内突变（开关蓝牙），不自动开；与 RSSI_STRONG 同为 -80
#define RSSI_SUDDEN_STRONG    -80
#define RSSI_FAR_CLOSE        -90     // 离场关门：≤此约走出 10m（开门时可略调 -88~-92）
#define BLE_CLOSE_FAR_SCANS   2       // 连续 N 次 ≤ FAR 才关（防抖）
#define BLE_SILENT_GAP_MS     8000    // 多久没匹配算「无」（原 15s，偏晚）
#define BLE_MISS_FOR_LOST     2       // 连续 N 次未匹配算「丢」（原 3）
#define BLE_TRACK_INTERVAL_MS 4000    // 跟踪扫描间隔（原 6000，反应更快）
#define BLE_TRACK_SCAN_MS     1500    // 单次扫描时长
// 离场关门：进入「强→弱/离开」后，最长等这么久就关（观察期可再调）
// 另：无→有立刻开门；误开问题后续再收紧
#define LEAVING_CLOSE_MS      10000
// ===== 关门：至少 3 个有效 RSSI 且单调变弱才「离开合格」=====
// 例：-60,-70,-80 可关；-60,-80,-60 视为跳动，否决关门
#define RSSI_TREND_MIN_N      3     // 最少样本
#define RSSI_TREND_DROP_DB    15    // 首末至少弱这么多 dB
#define RSSI_TREND_TOL_DB     3     // 相邻允许的小反弹（多径）
// 连续无信号这么久才算「真无」，之后再出现才允许无→有开（抖动 miss 不算无）
// 注意：仅用于开门门槛；不再用于「无信号 35s 兜底关门」（车熄火会突然消失）
#define RSSI_TRUE_SILENT_MS   20000
// （已废弃作自动关）曾用于无渐离信号消失 35s 后强关 → 车库内熄火误关
// #define RSSI_LEAVE_SILENT_MS  35000

// ===== NFC 读卡（与蓝牙共存）=====
// 跟踪期不再把 poll 间隔拉到 1200ms：改「蓝牙忙时不 poll + 空窗 350ms 密扫」
#ifndef NFC_POLL_GAP_BT_TRACK_MS
#define NFC_POLL_GAP_BT_TRACK_MS 350
#endif
// readPassiveTargetID 等待：过短（80ms）贴卡易漏；固定给足窗口
#ifndef NFC_READ_TIMEOUT_MS
#define NFC_READ_TIMEOUT_MS 200
#endif
// 慢 ACK 后立刻重试的间隔（卡可能还贴着）
#ifndef NFC_SLOW_RETRY_MS
#define NFC_SLOW_RETRY_MS 80
#endif
// 连续慢 ACK 次数 → 总线 resync
#ifndef NFC_SLOW_STREAK_RESYNC
#define NFC_SLOW_STREAK_RESYNC 3
#endif
// 连续无卡 N 次才刷新 RF 场（过勤会打出慢 ACK 死循环）
#ifndef NFC_FIELD_REFRESH_POLLS
#define NFC_FIELD_REFRESH_POLLS 30
#endif
// （保留宏兼容；场刷新不再按短时间触发）
#ifndef NFC_FIELD_REFRESH_MS
#define NFC_FIELD_REFRESH_MS 30000
#endif
// 自动关码最小间隔：防 leaveQual 卡住后每 4s 连发关码堵死门机
#ifndef AUTO_CLOSE_MIN_INTERVAL_MS
#define AUTO_CLOSE_MIN_INTERVAL_MS 20000
#endif

// ===== 经典蓝牙（与 BLE 共用开/关状态机阈值）=====
// 开/关逻辑与 BLE 相同：无→有开；首见≥RSSI_SUDDEN_STRONG 不开；强→弱→无关
// 以下仅保留给趋势/分区调试，门控不再单靠渐变
#define RSSI_OPEN             -80     // （旧渐近开阈值，门控已统一）
#define RSSI_FADE             -90     // 渐离弱信号阈值
#define SLOPE_MIN             0.6f    // 渐变最小斜率 dBm/s
#define T_CLEAR_MS            100000  // 门洞清空等待 100s
#define T_SILENT_GAP_MS       10000   // 间隔多久算「突然出现」
#define T_LOSS_BLIP_MS        4000    // 连续丢包多久算「突然消失」

// ===== 跟踪模式（二选一）=====
// BLE_MODE: SU7 等有 BLE 广播的车
// CLASSIC_MODE: 小蚂蚁等仅经典蓝牙的车
#define TRACK_MODE_BLE        0
#define TRACK_MODE_CLASSIC    1
#define TRACK_MODE_DEFAULT    TRACK_MODE_BLE

// 开门冷却：防误开砸车（开了车没进去又触发开）。正常回家间隔远超此值
#define AUTO_COOLDOWN_OPEN_MS 300000  // 5 分钟
// 关门冷却：车走了及时关，不需要长
#define AUTO_COOLDOWN_CLOSE_MS 30000  // 30 秒
// 自动开后至少等这么久才能自动关（下车走出去约需十几秒；过短易关到人）
#define AUTO_MIN_OPEN_HOLD_MS 20000   // 20 秒（原 60s，离场关门偏晚）
// 上电宽限：此时间内禁止自动关（防第二块板启动即连发 close）
#define AUTO_BOOT_GRACE_MS 15000
// 手动 NFC/串口/网页 开关后，只短时屏蔽自动「开」（防手动关完立刻被无→有顶开）
// 离场自动关不受此限制；真车场景：刷卡开完 10 秒就可能开走
#define MANUAL_SUPPRESS_MS 5000

// 串口调试
#define SerialBaud 115200

// ===== millis 回绕安全比较（uint32 约 49.7 天溢出）=====
// 禁止写成 millis() >= deadline；统一用下面的有符号差比较
static inline bool millisReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}
static inline bool millisBefore(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) < 0;
}
static inline bool millisNotAfter(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) <= 0;
}
