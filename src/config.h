#pragma once

// ===== 车库门控制器 P0 配置 =====
// 引脚按 ESP32 DevKit / WROOM32 常见接法，可按实际改

// 继电器（干接点脉冲，模拟墙控/遥控按键）
// 继电器控制脚：默认 G21（右列，避开紧挨的 GND）
#ifndef PIN_RELAY
#define PIN_RELAY 21
#endif
// 五步对照实测：低=吸合亮，高=释放灭 → 低电平触发
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif

// 门磁输入（上拉，闭合=门关）
#ifndef PIN_DOOR_MAGNET
#define PIN_DOOR_MAGNET 27
#endif

// 米家插座路径：TRIG 干接点输入（上拉，短接到 GND=触发）
#ifndef PIN_TRIG_IN
#define PIN_TRIG_IN 25
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

// RF 抓包参数
#define RF_CAPTURE_MAX_PULSES  512     // 最大记录脉冲数
#define RF_CAPTURE_GAP_US      10000   // 静默超过 10ms 视为一次传输结束
#define RF_CAPTURE_TIMEOUT_MS  5000    // 抓包超时 5 秒

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

// ===== 信号与安全参数 =====
#define RSSI_SAMPLE_MS        1000
#define RSSI_WINDOW           10      // 滑动窗口点数
#define RELAY_PULSE_MS        2000    // 遥控按压时长；1s 电机常不认，改 2s

// ===== 简化 BLE 状态机（无→有→强=开；强→弱→无=关）=====
// 用于 SU7 等有 BLE 广播的车
// 实测参考（ESP32 在库内靠门侧）：
//   车在库外远处 ~-97；库外门口 ~-75~-81；库内强 ~-70 以上
#define RSSI_APPEAR_MIN       -110    // 出现信号下限（≥此值算「有」）
#define RSSI_STRONG           -80     // 强信号阈值（≥此值算「强」）
#define BLE_SILENT_GAP_MS     15000   // BLE 多久没匹配算「无信号」
#define BLE_MISS_FOR_LOST     3       // 连续 N 次未匹配算「丢」

// ===== 经典蓝牙渐变规则（用于小蚂蚁等无 BLE 车型）=====
// 实测待补充：隔车库门经典蓝牙 RSSI 大约多少
#define RSSI_OPEN             -80     // 渐近开门阈值 dBm（隔门信号弱，需实测调整）
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
// 自动开后至少等这么久才能自动关（给车进库时间）
#define AUTO_MIN_OPEN_HOLD_MS 60000   // 1 分钟

// 串口调试
#define SerialBaud 115200
