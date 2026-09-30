---
feature: dda0-btu-fail-zero
status: in-progress
updated: 2026-09-30
branch: fix/dda0-btu-fail-zero
commits: 7a50a16..
---

# dda0 BTU fail 归零

> 项目约定：所有改动与 OTA 验证过程 commit 到分支 `fix/dda0-btu-fail-zero`，验证通过后再考虑合并 master。工作区为当前主仓该分支（用户明确要求新分支，不建 worktree）。

## Report

## [S1] Problem

目标：`garage-dda0` 的 BTU fail 速率降到 **每 10 分钟 ≤ 2 次**（日志 `[BTUFAIL]` / `BTSTAT.btufail` 口径）。

当前实测（固件 `0.2.202609302318`，2026-09-30）：

| 设备 | 最近 BOOT | 观察窗 | btufail | thin | inq | 约次/10min | maxblk 特征 |
|------|-----------|--------|---------|------|-----|------------|-------------|
| garage-dda0 | 23:31:54 rst=1 | 23:32–23:44 ~12min | 53 | 53 | 185 | **~44** | 常 <4112（660~2932），偶发 5620 |
| garage-1388 | 23:22:44 rst=3 | ~22min | 2 | 1 | 326 | **~0.9** | 常 4340/8180 |

dda0 关键现象：

1. `thin` 与 `btufail` **几乎 1:1**：inquiry 前 `largest8 < 4112` → BTU `malloc(4112)` 必挂。
2. BT air 在 boot 时 `force hold 8192`（当时 max8=51188），第一次 inquiry `drop` 后 max8 只剩 ~8180，**20s 内碎到 2292**。
3. BT air **几乎不再 re-hold**（`btAirTryHold` 要求 largest≥10000，dda0 稳态达不到）。
4. OTA 共用气囊在 inquiry 前 `Give` 掉，且 `autoTrack` 期间禁止 `Rearm` → **失败后没有任何应急连续块可给 BTU**。
5. 同步 `HEAPFAIL`：`BTU_TASK sz=4112 big8=2292` 与 `wifi sz=2308` 同窗口；另有 `datagate/netfail → force STA reconnect`，持续打碎堆。

根因：**不是 inquiry 太密，而是 dda0 堆在 inquiry 窗口碎到 <4112 后，现有气囊策略“一次给完就没了”，BTU 无应急预留。**

## [S2] Design

### 验收口径

- 主设备 `garage-dda0`：OTA 后连续观察 **≥15 分钟**，`[BTUFAIL]` / `BTSTAT.btufail` 增量 **≤2 次/10min**（按 btufail 增量与时间窗计算）。
- 对照设备 `garage-1388`：先 OTA 同版本验证，**不得回归**（btufail 速率仍 ≤2/10min，且无新的崩溃/离线/门控异常）。
- 过程：**编译 → commit → publish OTA → 按设备 `POST /api/devices/{id}/update` 先 1388 后 dda0 → 看 VPS 日志**。1388 失败则回退改代码；dda0 仍失败则从分析开始再迭代。

### 修复契约（方向 A：BTU 专用应急堆）

在现有「BT air + OTA 共用气囊 + inquiry 静默 + HTTP/log 堆薄暂停」之上，增加**一块不与 OTA 共用、默认不释放的 BTU 应急连续块**：

1. **分配**
   - `BleTracker::begin()` 在 BT 栈 ready 后，与 `btAirTryHoldForce()` 同时 `heap_caps_malloc(kBtuReserveSize, MALLOC_CAP_8BIT)`。
   - `kBtuReserveSize = 8192`（≥ BTU 4112，且给 WiFi 2308 抢走后仍可能剩 ≥4112 的连续区）。
   - 用 `MALLOC_CAP_8BIT`，与 BTU/WiFi 实际失败池一致（不用 `ESP.getFreeHeap()` 的 INTERNAL 口径）。

2. **释放时机（只在需要时给）**
   - `ensureBtuHeapForInquiry()`：先维持现状（OTA Give + BT air drop）。
   - 若此后 `largest8 < 4112` 且应急块仍持有 → **立刻 free 应急块**，再查一次 `largest8`。
   - 若 free 后仍 `< 4112` → `noteThin()` 并照旧启动 inquiry（不推迟自动开门，但会尽量少 fail）。
   - `onAllocFailed` 若判定为 BTU/4112：置位 `gBtuResGiveReq`；loop 里若应急块仍在则 free，供**下一轮** inquiry。

3. **收回时机（防再次打碎 BTU 空间）**
   - 仅当 inquiry 结束后静默窗结束、且 `largest8 >= 12288` 时 `rearm` 应急块。
   - 稳态 maxblk≈4340/5620 时**禁止** rearm（否则 4340−4608/8192 会立刻把 BTU 又饿死）。
   - `autoTrack` 期间继续禁止 OTA 气囊 rearm（保持现状）。

4. **配套**
   - `POST_INQUIRY_QUIET_MS`：`800 → 1500`，给 BTU 异步 malloc + 减少静默窗内数据面抢块。
   - inquiry 前堆薄路径：继续暂停 `log/status/remoteCmd`（已有）；应急 free 后若仍薄，**同一 inquiry 内不再二次分配大包**。
   - 1388 健康路径：`largest8≥4112` 时**不释放**应急块，避免无谓碎片与行为回退。

5. **OTA / 验证流程**
   - 版本号继续走 `bump_version.py` → `FW_VERSION`，与 `version.json` 一致。
   - 发布：编译 → `publish_ota.ps1`（或等价 scp）→ 对 **garage-1388** 先 `POST /api/devices/garage-1388/update`。
   - 1388 日志：`[OTA]` 版本变化 + ≥15min 无回归。
   - 通过后再对 **garage-dda0** 下发同版本 update。
   - dda0 日志：`[BTUFAIL]` 增量 ≤2/10min；若仍超，回退/改代码再走循环。
   - 每次固件改动必须 `git commit`（可追溯）。

6. **日志契约**
   - 新增/保留可观测字段：
     - `[HEAP] BTU reserve held/dropped/rearmed sz=… max8=…`
     - `[BTUFAIL]` / `[BTSTAT]`（已有）
     - thin 时仍打 `[HEAP] BT thin before inq … btufail=…`
   - VPS 侧统计脚本可只用 `device-garage-dda0-*.log` 中 BTSTAT/BTUFAIL 时间戳。

### 验证与回退

| 阶段 | 通过条件 | 失败动作 |
|------|----------|----------|
| 编译 | `platformio run -e esp32dev` 成功 | 修代码，不 OTA |
| 1388 | 版本上升 + 无新 panic/离线 + btufail≤2/10min | 回退本次改动（git revert / 重写），再分析 |
| dda0 | 同上，且门控/NFC/远程令不劣化 | 从日志重分析，下一轮最大可能方向 |

## [S3] Out of Scope

- 不 USB/COM3 烧录（OTA 验证；OTA 自身损坏或设备完全离线才另议）。
- 不合并 master、不 push 远程（用户明确：先在新分支，没问题再合并）。
- 不把「总 HEAPFAIL（含 WiFi 2308）」列为本轮硬验收——只收口 **BTU fail**；WiFi 失败仅作相关观察。
- 不改门控/RF/NFC 业务语义；不主动降低 inquiry 频率到影响自动开门（除非后续证据证明必须）。
- 不处理 dda0 偶发离线的全部网络问题（仅在与 BTU 堆相关时记录）。

## Tasks

- [ ] T1: 分析 dda0/1388 当前 BTU fail 与堆日志 — acceptance: Spec 附有实测数字与根因，且确认 thin≈btufail、1388 已达标 (covers: S1)
- [ ] T2: 实现 BTU 专用应急堆 + quiet 延长 + 释放/收回契约 — acceptance: `ble_tracker.*` 有 reserve held/dropped/rearmed 日志；thin 时释放 8192；`largest8<4112` 才释放；≥12288 才 rearm；POST_INQUIRY_QUIET_MS=1500 (covers: S2)
- [ ] T3: 编译固件并 commit 到 `fix/dda0-btu-fail-zero` — acceptance: `platformio run -e esp32dev` 成功；`FW_VERSION` 更新；git 有可追溯 commit (covers: S2; depends: T2)
- [ ] T4: OTA 发布并先验证 garage-1388 — acceptance: 1388 fw 变为新版本；≥15min 日志 btufail≤2/10min 且无回归；问题则回退重改 (covers: S2; depends: T3)
- [ ] T5: OTA 到 garage-dda0 并验证 BTU 归零 — acceptance: dda0 fw 升级；≥15min `btufail` 增量≤2/10min；若仍超则迭代 (covers: S2; depends: T4)
