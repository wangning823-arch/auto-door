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
- **自动开/关（两通道）**
  - **BLE 手机**：仅已配对 IRK+RSSI；无→有且 **&lt; -80** 开；**≥ -80** 不开；离场 **≤ -90** 约 10m 关
  - **经典车机**：固定 MAC + 同样 RSSI 阈值（小蚂蚁等）
  - **已移除**：BLE 名称/MAC 特征扫描与 filter 通道
  - 详见 `docs/联调记录_20260919_RF学习与BLE自动门.md`
- **冷却保护**：开门 5min / 关门 30s / 开后保持 60s
- **315/433 固定码**：**key0 开 / key1 关每次上电强制为同一套已验证码**（多板一致）；key2/3 仍可 NVS 学习，但 `rflearn 0/1` 重启后会被默认开/关码覆盖
- **NFC 上电自恢复**：约 5s 后自动 `nfcinit`；失败则约每 10 分钟慢速重试（最多约 20 次）；网页有「重新初始化 NFC」
- **Inquiry 卡死兜底**：经典蓝牙 busy 超过约 8s 强制取消并清标志，避免跟踪永久停死
- **millis 回绕安全**：调度截止时间统一用有符号差比较，降低约 50 天后停扫/停刷卡风险
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

## 在线升级（桌面 OTA 客户端）

**前置**：网页「家庭 Wi‑Fi」已填 2.4G SSID/密码，首页显示 `OTA: garage-xxxx.local`；设备与电脑同一局域网。

1. 先用 USB 烧一次带 `/ota` 与 `FW_VERSION` 的新固件（之后才能读版本号）
2. 双击 `tools\OTA升级.vbs`（或 `OTA升级.bat`）
3. 填 `xxxx`（或完整主机名 / STA IP）→ **检测在线**
4. 看状态：在线、固件版本、OTA 服务、能否更新
5. **先编译**（或选已有 `firmware.bin`）→ **开始更新**
6. 进度走完后客户端会轮询设备恢复；成功则显示新版本号

| 字段 | 说明 |
|---|---|
| 固件版本 | 来自 `config.h` 的 `FW_VERSION`（改版本再编译，升级后才好区分） |
| 编译时间 | 设备内嵌的 `__DATE__ __TIME__` |
| 能否更新 | STA 已连 + ArduinoOTA 已 begin（或 TCP 3232 通） |
| 接口 | 新固件 `GET /ota` JSON；旧固件回退探测首页 |

命令行等价：

```powershell
# 改 platformio.ini 里 upload_port，或：
& $env:MIMO_PYTHON -m platformio run -e esp32dev_ota -t upload --upload-port garage-xxxx.local
```

版本号由 `tools/bump_version.py` 在**每次编译前**自动写入 `src/fw_version.h`：

- 格式：`0.2.YYYYMMDDHHmm`（例：`0.2.202609221831`）
- 时间 = `src/` 下源码最新 mtime → **改过代码再编译才会升号**
- 不要手改 `fw_version.h` / 也不要再在 `config.h` 写死 `FW_VERSION`

## 配置方式

1. 连热点 `GarageDoor-xxxx` / 密码 `12345678`
2. 浏览器打开 `http://192.168.4.1/`
3. 选择跟踪模式（BLE / 经典蓝牙）
4. BLE：扫描并点选设备特征
   经典蓝牙：保存车机 MAC
5. 可选：关闭 WiFi（释放射频给蓝牙）

### WiFi 调试开关（`config.h`）

| 宏 | 行为 |
|---|---|
| `WIFI_DEBUG_BOOT_ON=1`（当前） | 网页可关 WiFi，但**重新上电会自动再开热点** |
| `WIFI_DEBUG_BOOT_ON=0`（稳定后） | 尊重 NVS：关掉后需串口 `wifi on` / BOOT 长按 3s |
| `WIFI_AP_YIELD_BT=1`（当前） | **热点打开期间蓝牙让射频**：启动静默 `WIFI_AP_BOOT_QUIET_MS`（默认 45s）停 Inquiry/BLE；之后热点仍开则 Inquiry 慢速、且不跑阻塞 BLE 扫描 |
| `WIFI_AP_BOOT_QUIET_MS` | SoftAP 启动后的射频静默窗口（毫秒） |

原因：ESP32 单射频；手机**关联完成前** `softAPgetStationNum()` 常为 0，旧逻辑只在「已有客户端」时降级蓝牙，导致热点「时有时无、连上打不开网页」。测自动门仍应关掉 SoftAP。

## RF 发射安全（防堵门机）

| 机制 | 行为 |
|---|---|
| 上电不恢复 `rfauto` | NVS 若为 ON → **强制 OFF 并清除** |
| `rfauto` 会话超时 | `RF_AUTO_MAX_MS`（默认 120s）到时自动 OFF |
| TX 卡死看门狗 | 空闲期 DATA 持续高电平 `RF_TX_STUCK_MS` → 强制拉低 |
| 抓包工具 | 就绪后发 `rfauto off`，**不再**自动 `rfauto on` |

现象复盘：抓包工具自动 `rfauto on` 写 NVS → 上电恢复 → 整夜每 5s 发 315 开码 → 门机接收被堵 → 原遥控只能贴很近、刷卡也失效；拔掉 ESP32 即恢复。

## 串口命令

```
status          查看状态
open / close    手动开关
rfauto on|off   周期自动发开码（默认 OFF；最长 2 分钟自动关）
                ⚠ 整夜开着会用 315/433 堵死门机接收：原遥控/刷卡全失效，拔 ESP32 才恢复
rflearn 0-3     学习按键（0开 1关 2暂停 3锁）
rfplay 0-3      回放按键
rfset 0 <csv>   手动灌码
rfcap / rfstop  连续抓包
rfkeys/rfexport 查看/导出按键
rfdefaults      写入实车验证的默认开/关码
ble [sec]       BLE 扫描
bletrack on/off BLE IRK 跟踪（仅已配对手机）
blefilter       已移除名称/MAC 特征通道（请用 blepair 配对）
blebond         配对/IRK 状态
blepair [sec]   打开配对窗口（默认 90s；`blepair 0`=保持开）；网页也有开关
blepair off     关闭配对
bleunpair       清除已授权手机
blepin xxx      设置/清空配对密码（网页开窗时校验；空参数=清除）
autotrack on/off 经典蓝牙自动跟踪
wifi on/off     WiFi 开关
remote on/off   VPS 轮询远程开（蓝牙空隙才访问 WiFi）
```

### 远程令 MVP（小爱 → VPS → ESP32）

- 服务端：线上 `https://door.wzx.homes`（VPS 上 `garage-gate` + nginx 反代 `127.0.0.1:18080`）
- **MCP**：`https://door.wzx.homes/mcp`（Streamable HTTP POST + 旧 SSE GET）；工具 `open_garage` / `close_garage` / `garage_status`
- 本地调试服务端：`../vps/garage_gate.py`（Python3 标准库，零依赖）
- 固件轮询：`src/remote_cmd.*`；**默认关**，串口 `remote on` 打开
- `config.h` 默认 `REMOTE_POLL_URL=http://door.wzx.homes/dev/poll`（**明文 HTTP**：TLS 在 BT+STA 下堆不够）
- nginx：仅 `location = /dev/poll` 允许 80 口明文反代；`/mcp`、`/xiaoai/*`、`/health` 仍 HTTPS
- **在线 OTA**：`/dev/poll?fw=…` 与 `ota/version` 比对，落后则回 `{"cmd":"update"}` 立刻升级；另每天兜底查一次。**日常发布用一键脚本**（不必 USB 烧录）：
  ```powershell
  powershell -File D:\mimo\车库门自动化\.mimocode\skills\garage-ota-release\scripts\publish_ota.ps1
  powershell -File ...publish_ota.ps1 -TailLogs   # 看设备远程日志
  ```
  手动催更：`POST https://door.wzx.homes/xiaoai/update`；串口 `ota check`。流程详见技能 `garage-ota-release`
- **日志上报**：约 30s 推到 `/dev/logs`，VPS 存 `/opt/garage-gate/logs/device-YYYYMMDD.log`（保留 14 天）。串口 `logs flush`
- **手机开关门页**：`https://door.wzx.homes/`（密码在 VPS `/opt/garage-gate/ui_password`）
- 串口 `remote on/off`；日志看 `[REMOTE]`
- 蓝牙优先：Inquiry / BLE 扫描进行中**绝不**发 HTTP；STA 已连才轮询
- 冒烟：`curl -X POST https://door.wzx.homes/xiaoai/open` 后设备应打出 `[REMOTE] cmd=open`

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
