---
feature: nfc-phone-tap-command
status: in-progress
updated: 2026-09-25
branch: master
commits: 086d0df..
---

# NFC 刷手机：弹窗但不发命令

> 项目约定：继续在主仓 master 推进（历史修复与 OTA 发布脚本均绑定此树）；产物只 OTA 到 garage-1388，dda0 在 ota_hold。

## Report

## [S1] Problem

刷手机时 NFC 正常弹窗（RF 场在），但不触发开门命令。串口持续：

```
[NFC] inlist err ir=-1 cost=1007ms streak=N open=0
```

甚至 `cost=3002ms`。VPS 侧只有 `PN532 ready`，无任何 `card:` 行。授权卡 `ABDDE836` 同样从未读出。

## [S2] Design

根因（已由串口/代码对齐）：

1. **每轮 poll 都重发 InList**。手机 HCE 激活需要「场连续 + 同一条 InList 不中断」；超时/空窗后立刻 `WriteCmd(InList)` 会重置 ATR → 弹窗却永远没有 UID 帧。
2. **超时路径 `pn532Drain()`** 会把随后到达的出卡帧扔掉，下一轮再叠发 InList，形成 `ir=-1` 风暴。
3. `s_inlistOpen` 曾被完全绕开（注释写明「每轮都发新 InList」），芯片侧寻卡被反复打断。

修复契约（最小改动）：

- InList 改为**粘滞**：`s_inlistOpen==true` 时只 `ReadFrame` 等出卡/0-tags，**禁止**写新 InList、禁止 drain。
- `NFC_INLIST_RETRIES=0xFF`：片上一直寻到卡；空场不结束命令，等价于持续寻卡。
- 超时且仍 open → 返回「本轮无卡」（ir=0），下一轮继续读同一条 InList。
- 仅当收到完整帧后才 `s_inlistOpen=false`；0-tags 或解析失败才允许下一轮重发。
- 粘滞超过 `NFC_INLIST_STUCK_MS` 仍无 ready → `setRetries(0x01)+drain` 打断再重发，防止假死。
- 空场刷新 RF 仅在 `!s_inlistOpen` 时做，避免 RFConfiguration 打断进行中的 InList。
- 不改授权/门控/OTA 流程；失败路径继续 `Wire.end` + 短超时。

## [S3] Out of Scope

- 不动 garage-dda0，不摘 ota_hold。
- 不改 BLE/RF/门控状态机。
- 不做手机 HCE AID 层认证（仍按 UID）；若手机 UID 与 `ABDDE836` 不同，属注册问题，不靠本次代码吞掉。

## Tasks

- [ ] T1: InList 粘滞化 + 超时不 drain — acceptance: 串口不再连续 `inlist err ir=-1 cost=1000ms+`；空场为 `等卡中` 或低 cost 等待 (covers: S2)
- [ ] T2: 编译并只 OTA 到 garage-1388 — acceptance: 日志出现新版本 `PN532 ready`，dda0 版本不变 (covers: S2)
- [ ] T3: 贴卡/手机验收 — acceptance: 串口出现 `card: …`；授权卡触发 `authorized → RF` (covers: S2; depends: T2)
