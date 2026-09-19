# 车库门智能控制器 P0 固件

对应商品方案 `docs/商品方案_学习型智能车库门控制器.md` 的 P0 原型。

## 目录结构

```
garage_door_firmware/
├── src/              # 固件源码
├── docs/             # 文档、接线图
├── tools/            # 调试工具（RSSI监控、串口脚本）
├── platformio.ini
└── README.md
```

## 核心功能

- **跟踪模式二选一**：BLE / 经典蓝牙（网页切换，存 NVS）
- **自动开/关（BLE 与经典同一套状态机）**：
  - 无 → 有即开（弱信号也开，例如门口约 -93）
  - 首见 ≥ -70 不开（库内蓝牙唤醒）
  - 强(-80) → 弱 → 无 = 关
  - 详见 `docs/联调记录_20260919_RF学习与BLE自动门.md`
- **冷却保护**：开门 5min / 关门 30s / 开后保持 60s
- **315/433 固定码**：**key0 开 / key1 关每次上电强制为同一套已验证码**（多板一致）；key2/3 仍可 NVS 学习，但 `rflearn 0/1` 重启后会被默认开/关码覆盖
- 手动开关（网页/串口/NFC/米家 TRIG）
- **不用门磁**：开/关为不同 RF 码，门态只按本机发码后的软件状态维护

## 接线（默认，可在 `config.h` 改）

| 功能 | GPIO |
|---|---|
| 继电器（开漏，低=吸合） | 21 |
| ~~门磁~~ | 已停用 |
| TRIG 米家（上拉，对地触发） | 25 |
| 学习键（板载 BOOT） | 0 |
| 状态 LED | 2 |

继电器需独立 5V 供电，与 ESP32 共地。

## 编译烧录（PlatformIO）

```powershell
cd D:\mimo\车库门自动化\garage_door_firmware
& $env:MIMO_PYTHON -m platformio run -t upload
& $env:MIMO_PYTHON -m platformio device monitor -b 115200 -p COM3
```

## 配置方式

1. 连热点 `GarageDoor-xxxx` / 密码 `12345678`
2. 浏览器打开 `http://192.168.4.1/`
3. 选择跟踪模式（BLE / 经典蓝牙）
4. BLE：扫描并点选设备特征
   经典蓝牙：保存车机 MAC
5. 可选：关闭 WiFi（释放射频给蓝牙）

## 串口命令

```
status          查看状态
open / close    手动开关
rfauto on|off   周期自动发开码（默认应 OFF）
rflearn 0-3     学习按键（0开 1关 2暂停 3锁）
rfplay 0-3      回放按键
rfset 0 <csv>   手动灌码
rfcap / rfstop  连续抓包
rfkeys/rfexport 查看/导出按键
rfdefaults      写入实车验证的默认开/关码
ble [sec]       BLE 扫描
bletrack on/off BLE 跟踪开关
blefilter XXX   设置 BLE 特征
autotrack on/off 经典蓝牙自动跟踪
wifi on/off     WiFi 开关
```

## 调试工具

- `tools/学习按键.bat` / `rf_learn_gui.py` - 按键学习向导
- `tools/RF.bat` / `rf_capture_viewer.py` - 连续抓包与波形
- `tools/rf_cmd.py` - 串口发命令
- `tools/open_key_candidates.json` - 开键多帧聚类候选
- `tools/rf_keys_backup.json` - 按键备份（key0开/key1关已实车验证）
- `docs/联调记录_20260919_RF学习与BLE自动门.md` - 本轮联调说明

## 待实现（商品化版本）

- 无感标定：日常使用自动学习 RSSI 阈值（见商品方案文档 §10.5）
- NFC 刷卡
- 米家语音
- （可选）门磁联锁：当前固件已停用门磁，开/关靠不同 RF 码

## 原固件备份

`../firmware_backup/`（仓库外）：已保存 `partitions.bin`、`nvs.bin`。

**烧录本固件会覆盖 OTA app 分区，原 AT 固件将不可用。**
