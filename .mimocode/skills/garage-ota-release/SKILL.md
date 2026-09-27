---
name: garage-ota-release
description: 发布车库门 ESP32 固件新版本并通过 VPS 在线 OTA 与远程日志调试。Use when 用户说「发布新版本」「OTA 发布」「推送固件」「推到车上」「不用烧录」「别烧录」「远程调试」「看设备日志」「看远程日志」「等 OTA」，或完成 garage_door_firmware 改动后要把新固件交付到实车设备。Do NOT use for 继电器接线、NFC/RF 硬件问题、纯编译报错、或用户明确要求 USB/COM3 烧录。
---

# 车库门固件 OTA 发布与远程调试

## 重要规则

1. **完成固件改动后默认不要 `platformio -t upload` / USB 烧录**，改为 OTA 发布。
2. 调试优先查 **VPS 日志**，不必守在车库。
3. 仅当 OTA 自身坏了、设备完全离线、或用户明确要求时，才用 COM3 烧录。
4. 发布用的 `version.json` 的 `version` 必须等于固件内 `FW_VERSION`（`src/fw_version.h`），否则会反复升级。

## 发布流程

### Step 1：编译

在 `garage_door_firmware/`：

```powershell
& $env:MIMO_PYTHON -m platformio run -e esp32dev
```

编译前会自动写入 `src/fw_version.h`（格式 `0.2.YYYYMMDDHHmm`）。记下版本号。

### Step 2：一键发布到 VPS

```powershell
powershell -File D:\mimo\车库门自动化\.mimocode\skills\garage-ota-release\scripts\publish_ota.ps1
```

脚本会：
1. 确认 `firmware.bin` 存在  
2. `scp` 到 VPS `/opt/garage-gate/ota/firmware.bin`  
3. 在 VPS 上按 **实际 bin 的 sha256** 写 `version.json`（version=FW_VERSION）  
4. **不自动下发** `update`（已改为手动）— 需在网页设备页点 **「立即更新」**

SSH：`root@101.37.175.30`，密钥 `~/.ssh/id_ed25519`。

### Step 3：网页点「立即更新」并等 OTA 完成

**升级只由网页「立即更新」触发**（或 `POST /api/devices/{id}/update` / `POST /xiaoai/update`）。上传固件、版本落后都不会自动升级。

设备约 3s 一轮 `/dev/poll`，收到 `update` 后：停 NFC/Inquiry → 下载并写入 → 校验 sha256 → 激活重启。一般 **1～2 分钟**。

看网关日志：

```bash
ssh -i ~/.ssh/id_ed25519 root@101.37.175.30 "journalctl -u garage-gate -n 30 --no-pager"
```

期望看到：

```text
update requested (xiaoai)
dev poll consumed cmd=update fw=...
```

### Step 4：用 VPS 日志调试（代替串口）

设备会把关键日志推到 `/opt/garage-gate/logs/device-YYYYMMDD.log`（约 30s 一批）：

```bash
ssh -i ~/.ssh/id_ed25519 root@101.37.175.30 "tail -50 /opt/garage-gate/logs/device-\$(date +%Y%m%d).log"
```

或：

```powershell
powershell -File D:\mimo\车库门自动化\.mimocode\skills\garage-ota-release\scripts\publish_ota.ps1 -TailLogs
```

关注：`[REMOTE] cmd=`、`[OTA]`、`[FSM]`、`[LOG] heap=...`。

若日志不够，再考虑连串口。串口命令：`logs flush` 立刻上报，`ota check` 立刻查版本。

## 触发方式对照

| 场景 | 做法 |
|------|------|
| 发布后立刻升级 | 发布 → 网页设备页点 **「立即更新」** |
| 只催一次 | 网页「立即更新」或 `curl -X POST https://door.wzx.homes/xiaoai/update` |
| 上传固件 | 网页「上传固件」/ `publish_ota.ps1`（只落服务器，不通知设备） |
| 无自动升级 | 版本号更了也不会自动推；设备端无定时检查 |

## Examples

用户：「改完了，发布一下 / 不用烧录 / 推给设备」  
→ 编译 → `publish_ota.ps1` → 网页设备页点「**立即更新**」→ tail 设备日志确认 `fw=` 新版本。

用户：「看下设备最近日志」  
→ SSH tail `device-*.log`，不要求接 USB。

**不会自动升级**：上传/发布只换服务器上的 bin；板子只在「立即更新」后才拉固件。

## Troubleshooting

| 现象 | 原因 | 处理 |
|------|------|------|
| 一直 `update` 循环 | `version.json` 的 version ≠ 固件 `FW_VERSION` | 用 `publish_ota.ps1` 重写 version.json，或改到一致 |
| 无 `cmd=update` | 设备没 poll / BT 忙 | 等 1～2 分钟；journal 里看 `dev poll` |
| `[OTA] bin fetch fail` | bin 未上传或 nginx 未放行 `/ota/` | 确认 `/opt/garage-gate/ota/firmware.bin`；`nginx -t && reload` |
| 日志没有新行 | 未到 30s 或 STA 未连 | 串口 `logs flush`，或等下一批 |
| OTA 后版本没变 | 只改了 version.json 没换 bin | 必须同时上传新的 `firmware.bin` |

## 相关路径

| 路径 | 说明 |
|------|------|
| `garage_door_firmware/` | 固件工程 |
| VPS `/opt/garage-gate/ota/` | `firmware.bin` + `version.json` |
| VPS `/opt/garage-gate/logs/` | 设备上报日志（保留 14 天） |
| `https://door.wzx.homes/` | 手机开关门页 |
| `https://door.wzx.homes/xiaoai/update` | 手动催 OTA |
