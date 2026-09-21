# PN532 NFC 联调踩坑记录

日期：2026-09-21  
硬件：ESP32 DevKit + PN532（I2C），**SDA=GPIO16、SCL=GPIO17（禁止对调）**  
串口：COM3，`dtr=False, rts=False` 打开以免触发复位

成功形态：

```
idle SDA=1 SCL=1
ver=0x32010607
setRetries ack=1
RF field on ack=1
PN532 ready
[NFC] card: <UID>
[NFC] authorized -> toggle
[FSM] RF TX open/up
```

---

## 1. SCL 锁死 0.04V（最致命）

**现象**：上电数分钟后 SCL 掉到约 0.04V，I2C 全死，`Error 263 (ESP_ERR_TIMEOUT)` 风暴。

**根因链**：
1. `Wire.setTimeOut` 过短（曾用 100ms）撞上 PN532 时钟拉伸 / `readPassiveTargetID` 等待；
2. 超时后不 `Wire.end`，ESP 外设与从机一起把 SCL 按住；
3. 后台每 15s 自动 `hwInit` 再撞 → 风暴，永不松手。

**正确做法**：
- 任何 init/poll 失败路径必须 `releaseBus()` = `Wire.end()` + 内部上拉；
- 失败后 `deferred_=true`，**禁止后台自动 init**，只允许串口 `nfcinit`；
- `forceInit` 失败同样 `deferred_=!ok`；
- 上拉：`gpio_set_pull_mode(..., GPIO_PULLUP_ONLY)`（Wire 默认仅开漏，无外挂上拉会漂）；
- 恢复：`i2cBusRecover` **先推拉顶 SCL** 再 9-clock（纯开漏顶不动被从机拉死的 SCL）。

**硬恢复**：整板断电 5s（16/17 可不拔）。热插拔毛刺大，优先断电复位。

**勿回退**：失败即 `Wire.end`；deferred 禁止自动 init。

---

## 2. RF field on ack=0（手机无弹窗）

**现象**：`RF field on ack=0`，手机贴上去**不弹 NFC 窗口**；ack=1 时才能弹窗。

**根因**：
1. 前一条命令（尤其 `setRetries`）响应**没读干净** → 下一条 `readack` 读到脏帧；
2. 库 `setPassiveActivationRetries` 只 `readdata(6)`，实际帧可能更长；
3. 自拼 `RFConfiguration` 后不读响应 / 读错长度，同样弄脏总线。

**正确做法**：
- 发 RF 前先 `pn532Drain()`（只认 ready 字节，短超时 50ms）；
- RF 响应用 `sendCommandCheckAck` + `readResponse` 整帧读走（约 10 字节）；
- 失败：`nfcRewire`（`Wire.end`→`begin`）后重试一次；
- **成功后也要 rewire**，否则下一条 poll ACK 会卡 ~1.3s；
- RF 失败**非致命**：`InListPassiveTarget` 会自行开场，但无持续场时手机弹窗/读卡都变差。

**验证**：日志必须出现 `RF field on ack=1`；ack=0 时先修 drain/时序，不要先怀疑天线。

---

## 3. poll 固定 cost≈1277ms

**现象**：`readPassiveTargetID`/ACK 每次恰好约 1276–1277ms，然后 `ret=0`。

**根因**：PN532 响应残留或 RDY 状态脏 → `waitready` + Wire 超时叠出 ~1.3s。

**正确做法**：
- cost>800 → 打日志 `poll ACK 慢 → rewire`，`nfcRewire` + `pn532Drain`，拉长 `nextPollMs_`；
- rewire 后下一轮常见 `cost=29ms` 正常无卡；
- `Wire.setTimeOut` 正常跑 **200ms**；init 期间可临时 1000，结束后必须收回；
- **禁止**在 listen/poll 里周期性裸发 RFConfiguration（与 InList 抢 ACK，ack 全 0）。

---

## 4. 首次 getFirmwareVersion 固定超时 ~1.4s

**现象**：`ver=0x00000000 cost≈1431ms`，retry 才 `0x32010607`。

**根因**：`nfc.begin()` 内 wakeup→SAMConfig 仍在忙，紧接着读 ver 超时。

**正确做法**：
- `begin()` 后 `delay(300)`；
- 失败走 **一次** recover + retry（推拉 recover → 再 begin → 再 ver）；
- 不要无限重试首读；retry 成功即可。

---

## 5. 并行编译 / 串口死循环（会死机）

**现象**：多次同时 `platformio run -t upload` → `nfc_reader.cpp.o` 缺失、`.xtensa.info` 损坏；COM3 busy 时疯狂重试 open/upload/python → **开满进程把电脑打死**。

**规则（必须遵守）**：
1. **禁止**并行多次 upload；
2. COM3 忙时**不要**循环重试，等一下或用户确认；
3. 构建坏了：删 `.pio/build/esp32dev`，**单次**  
   `& "$env:MIMO_PYTHON" -m platformio run -e esp32dev -t upload` 到 `[SUCCESS]`；
4. 串口脚本 `dtr=False, rts=False`；烧录时不要同时开串口；
5. 死循环苗头：先停手，读磁盘，单进程一次做完。

---

## 6. 读卡 / 手机弹窗

| 点 | 说明 |
|----|------|
| 手机要亮屏解锁 | 锁屏常不开 HCE/弹窗 |
| 无卡 `ret=0` 是常态 | 总线高时**绝不** recover |
| 授权卡 | `auth=ABDDE836`；其它 UID 打 `未授权卡` |
| 授权后链路 | `card` → `authorized -> toggle` → `MANUAL OPEN/CLOSE` → `RF TX` |
| 开关键 | key0=open 49脉冲 OK；key1=close 80脉冲 OK |
| 未注册卡 | `nfcsave <UID>`（串口） |

---

## 7. 串口诊断命令

```
nfcinit          # 强制重初始化（唯一允许的硬件 init）
nfcscan          # 30s 阻塞贴卡
nfcsave <uid>    # 注册授权卡
nfcclear         # 清授权卡
status           # 含 nfc=/auth=
rfkeys           # 查开/关码是否已学习
buspull/busfree  # 强制推拉 / Wire.end 松手
sclhold/sclrelease
i2cscan / i2cscan2
```

---

## 8. 关键代码位置

| 逻辑 | 文件 |
|------|------|
| early pullup / forceIdlePullups / recover / failRelease | `src/nfc_reader.cpp` |
| pn532Drain / nfcRewire / pn532RfFieldOn | `src/nfc_reader.cpp` |
| deferred 禁止自动 init | `maybeRecover` / `forceInit` |
| 授权卡→开关门 | `src/main.cpp` poll 分支 |
| RF 发射 | `rfEmitDoor` → `gRf.playKey` → `[FSM] RF TX` |
| 库 setRetries 补读响应 | `.pio/libdeps/.../Adafruit_PN532.cpp`（**libdeps 可能被清，以 git/说明为准**） |

---

## 9. 一次通过的联调顺序

1. 确认无并行 pio/python；COM3 空闲  
2. 单次 build+upload 到 SUCCESS  
3. 串口 `nfcinit` → 必须 `ver` + `setRetries ack=1` + **`RF field on ack=1`**  
4. `rfkeys` 确认 open/close 码在  
5. 贴授权卡 → 看 `card` + `RF TX open`；门开后再刷 → `RF TX close`  
6. 放几分钟/半小时再刷，看是否仍 `ack=1`、SCL 是否仍 1  
7. 异常时先看 SCL；锁死则断电 5s，不要连发 nfcinit

---

## 10. 明确不要再做

- 对调 SDA/SCL  
- 失败路径不 `Wire.end`  
- 后台自动反复 `hwInit`  
- 盲改大段 `old_string`（先 Read）  
- 并行 upload / COM3 死循环重试  
- poll 循环里裸发 RFConfiguration  
- 无卡就 `i2cBusRecover`（会弄死 RF 中的 PN532）  
- `Wire.setTimeOut` 长期停在 1000  
