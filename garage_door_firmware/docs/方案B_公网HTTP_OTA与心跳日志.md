# 方案 B：公网 HTTP OTA + 设备心跳/日志（规划，未实现）

状态：**设计稿 · 未写代码**  
日期：2026-09-22  
关联：局域网 `espota` 与桌面客户端见 `tools/ota_client.py`；本方案用于**跨子网/公网**升级与远程运维。

---

## 1. 背景与目标

| 现状 | 目标 |
|------|------|
| ArduinoOTA / espota：电脑 **主动连** 设备 `TCP 3232` | 设备 **主动连** 公网 HTTPS，不依赖路由/mDNS |
| 车库网是家里子网，跨段 mDNS 常不可用 | 任意能访问服务端的电脑均可触发/完成升级 |
| 日志主要靠串口 | 心跳 + 日志可留服务端，便于事后排障 |
| 版本靠本地编译时间戳 `fw_version.h` | 服务端可列出每台设备 **ID / 版本 / 是否在线** |

**非目标（本期不做）：** 替换现有局域网 espota（可并存）；实时秒级遥测；多租户 SaaS。

---

## 2. 总体架构

```text
[ESP32 车库]  --HTTPS 定时/按需-->  [公网服务端]
                                      |-- GET  /api/v1/devices     列表
[书房 PC / 手机] --HTTPS 查询-------> |-- GET  /api/v1/device/{id} 详情
[书房 PC]     --上传固件/触发------> |-- 管理面（可选）
                                      |-- GET  /fw/version.json
                                      |-- GET  /fw/firmware.bin
[ESP32]       --下载固件------------|
[ESP32]       --POST heartbeat/logs->|
```

- **升级路径**：设备拉取 `version.json` → 需要则 `firmware.bin` → 本地写入 → 重启 → 新 `fw` 心跳。  
- **运维路径**：设备推心跳/日志；人读接口；与升级解耦。

---

## 3. 设备身份

| 字段 | 来源 | 说明 |
|------|------|------|
| `device_id` | 现成 mDNS 后缀，如 `garage-a1b2` | 与网页/OTA 主机名一致，全局唯一（芯片 MAC 派生） |
| `fw` | `FW_VERSION`（`0.2.YYYYMMDDHHmm`） | 编译前自动生成 |
| `ip` / `sta` / `ota` | 现有 `/ota` 语义 | 本地仍可查；公网以心跳为准 |
| `token` | 出厂或首次配网写入 NVS | 所有上行请求带 `Authorization: Bearer …` |

心跳里建议固定带：`device_id`, `fw`, `build`, `uptime_ms`, `heap`, `sta`, `rssi`（可选）。

---

## 4. 服务端 API（草案）

> 路径前缀 `/api/v1`；除特别说明外均需 Bearer token。  
> 实现语言不限（Python/FastAPI、Go、Node 均可）。

### 4.1 心跳

```http
POST /api/v1/heartbeat
Content-Type: application/json
Authorization: Bearer <token>

{
  "device_id": "garage-a1b2",
  "fw": "0.2.202609221833",
  "build": "202609221833",
  "uptime_ms": 123456,
  "heap": 180000,
  "sta": 1,
  "ip": "192.168.2.50",
  "rssi": -60
}
```

响应：`200 {"ok":1}`  
服务端记录 `last_seen`；`now - last_seen < 3×interval` 视为 online。

**建议周期**：30s（可配置）；失败指数退避，本地队列最多 N 条。

### 4.2 设备列表 / 详情（人读）

```http
GET /api/v1/devices
GET /api/v1/device/garage-a1b2
```

```json
{
  "devices": [
    {
      "device_id": "garage-a1b2",
      "fw": "0.2.202609221833",
      "online": true,
      "last_seen": "2026-09-22T18:40:00+08:00",
      "ip": "192.168.2.50",
      "heap": 180000
    }
  ]
}
```

### 4.3 版本与固件下载

```http
GET /api/v1/fw/version.json
Authorization: Bearer <token>   # 或设备专用只读 token

{
  "device_id": "garage-a1b2",     # 可选：按设备灰度；省略=全局
  "fw": "0.2.202609230900",
  "sha256": "…",
  "size": 1796192,
  "url": "/api/v1/fw/firmware.bin",
  "min_hw": null
}
```

```http
GET /api/v1/fw/firmware.bin
```

- 设备逻辑：`remote.fw != local.fw` **或** 管理端强制标志 → 下载 → 校验 sha256 → `Update.begin/write/end` → reboot。  
- **鉴权**：bin 与 version 至少要 token；公网裸奔下载一律禁止。

### 4.4 日志上报

```http
POST /api/v1/logs
Content-Type: application/json
Authorization: Bearer <token>

{
  "device_id": "garage-a1b2",
  "fw": "0.2.202609221833",
  "seq": 1024,
  "level": "error",          // debug|info|warn|error
  "lines": [
    {"t": 123456, "msg": "[OTA] START ..."},
    {"t": 124000, "msg": "[NFC] poll ACK slow"}
  ]
}
```

- 设备：环形缓冲（如 32–64KB 或最多 200 行）；断网堆积，恢复后按 `seq` 补传。  
- 服务端：按 `device_id` + 日期分文件/入库；查询接口可后补 `GET /api/v1/device/{id}/logs`。  
- **级别策略**：默认 `warn`+`error` + 关键状态；`debug` 可开关，避免刷流量。

查询（可选二期）：

```http
GET /api/v1/device/garage-a1b2/logs?since=…&limit=200
```

---

## 5. 设备侧（固件）改动清单（将来）

| 模块 | 内容 |
|------|------|
| 配置 NVS | `api_base`、`device_token`（可沿用/扩展 `config_store`） |
| `http_ota` | 拉 version、下载写 flash、进度、失败回滚策略 |
| `heartbeat` | 周期 POST；与现有 STA 状态联动（无网跳过） |
| `log_ship` | 串口关键路径旁路一份到环形缓冲 + 上报 |
| 与 ArduinoOTA | **可并存**：局域网仍 espota；公网走 HTTP |

**注意：**

- 写 flash 期间沿用现有原则：**暂停 NFC/Inquiry**，避免 PN532/I2C 被拖死（与现 `gOtaActive` 同类门控）。  
- 仅 `STA` 已连接时上报/拉取。  
- HTTPS：ESP32 需正确时间（SNTP）或 pinned 证书策略；个人项目可先 HTTP+token 仅限实验，**公网务必 HTTPS**。

---

## 6. 服务端部署（将来）

| 项 | 建议 |
|----|------|
| 形态 | 单机 Docker / 云主机小服务 + 对象存储或磁盘存 bin |
| 鉴权 | 设备 token 与管理 token 分离；bin 下载也要鉴权 |
| 上传固件 | 管理端 `PUT` 或 CI 推送；自动生成 `version.json` + sha256 |
| 保留 | 日志按天保留 N 天；心跳只保留 last_seen 即可 |
| 密钥 | 勿进 git（已 ignore `.env` 等） |

---

## 7. 安全清单（实现时必须过）

- [ ] 所有写接口与 bin 下载 Bearer / 等价鉴权  
- [ ] HTTPS；禁止明文公网长期跑  
- [ ] token 可轮换；泄露可吊销  
- [ ] 固件 sha256 校验失败不写入运行分区  
- [ ] 日志不含 Wi‑Fi 密码等秘密  
- [ ] 速率限制（防刷心跳/日志）  

---

## 8. 与现有局域网方案关系

| | 局域网 espota（已实现） | 方案 B 公网 HTTP（本稿） |
|--|-------------------------|---------------------------|
| 触发 | 电脑 → 设备:3232 | 设备 → 服务端拉取 |
| 网络 | 同网段或可路由 IP | 设备能出网即可 |
| 版本查看 | 桌面客户端 `/ota` | 服务端 `devices` + 心跳 |
| 日志 | 串口 | 服务端集中保存 |
| 代码 | 已有 | **未实现** |

两者可长期并存：车库现场 USB/局域网调试，远程用方案 B。

---

## 9. 建议实施顺序（将来）

1. 服务端：heartbeat + devices 只读（先能“看见”设备）  
2. version.json + bin 下载 + 设备拉取升级（最小 OTA）  
3. 日志 POST + 管理查询  
4. 管理页/触发强更、灰度  
5. 收紧鉴权与 HTTPS 证书策略  

---

## 10. 本文档位置

- 路径：`docs/方案B_公网HTTP_OTA与心跳日志.md`  
- **不包含可运行实现**；实现时另开任务，并建议 bump 版本时间戳后发布。
