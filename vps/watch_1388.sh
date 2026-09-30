#!/bin/bash
# 每 30 分钟检查 garage-1388：rst=4 panic / NFC 状态 / log_ship 1036 是否起效
# 由 VPS cron 调用；结果追加到 watch1388.log
set -u
LOGDIR=/opt/garage-gate/logs
OUT="$LOGDIR/watch1388.log"
UPGRADE_TS="2026-09-30 10:40:25"   # 1388 升到 0.2.202609301036 的 BOOT rst=3 时刻
BASELINE_PANIC="2026-09-30 04:54:36" # 升级前最后一次 rst=4

TODAY=$(date +%Y%m%d)
YDAY=$(date -d "yesterday" +%Y%m%d 2>/dev/null || date -v-1d +%Y%m%d 2>/dev/null)
FILES=""
[ -f "$LOGDIR/device-garage-1388-$TODAY.log" ] && FILES="$FILES $LOGDIR/device-garage-1388-$TODAY.log"
[ -f "$LOGDIR/device-garage-1388-$YDAY.log" ] && FILES="$FILES $LOGDIR/device-garage-1388-$YDAY.log"
if [ -z "$FILES" ]; then
  echo "$(date '+%F %T') WATCH1388 NO_LOG" >> "$OUT"
  exit 0
fi

python3 - "$FILES" "$OUT" "$UPGRADE_TS" "$BASELINE_PANIC" << 'PY'
import sys
from datetime import datetime
from collections import Counter

files = sys.argv[1].split()
out_path = sys.argv[2]
upgrade_ts = datetime.strptime(sys.argv[3], "%Y-%m-%d %H:%M:%S")
baseline = datetime.strptime(sys.argv[4], "%Y-%m-%d %H:%M:%S")

lines = []
for path in files:
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                if "2026-" not in line[:20]:
                    continue
                try:
                    ts = datetime.strptime(line[:19], "%Y-%m-%d %H:%M:%S")
                except Exception:
                    continue
                lines.append((ts, line.rstrip()))
    except Exception as e:
        print("read err", path, e)

lines.sort(key=lambda x: x[0])
now = datetime.now()
uptime_h = (now - upgrade_ts).total_seconds() / 3600.0

# 升级后 rst=4
panics_after = [(ts, l) for ts, l in lines if ts >= upgrade_ts and "[BOOT] rst=4" in l]
# 升级前最后一次 rst=4（对照）
panics_before = [(ts, l) for ts, l in lines if baseline <= ts < upgrade_ts and "[BOOT] rst=4" in l]

# NFC 状态（升级后）
nfc_c = Counter()
last_nfc = None
last_nfc_ts = None
pn_ready_after = []
setfail_after = 0
dead_after = 0
for ts, l in lines:
    if ts < upgrade_ts:
        continue
    if "nfc=ok" in l:
        nfc_c["ok"] += 1
        last_nfc, last_nfc_ts = "ok", ts
    elif "nfc=defer" in l:
        nfc_c["defer"] += 1
        last_nfc, last_nfc_ts = "defer", ts
    elif "nfc=wait" in l:
        nfc_c["wait"] += 1
        last_nfc, last_nfc_ts = "wait", ts
    elif "nfc=nochip" in l:
        nfc_c["nochip"] += 1
        last_nfc, last_nfc_ts = "nochip", ts
    if "PN532 ready" in l:
        pn_ready_after.append(ts)
    if "FAIL setRetries" in l:
        setfail_after += 1
    if "DEAD" in l and "NFC" in l:
        dead_after += 1

# 最近一次心跳
last_log = None
for ts, l in lines:
    if ts >= upgrade_ts and "[LOG]" in l and "nfc=" in l:
        last_log = (ts, l)

hours_since_upgrade = uptime_h
if len(panics_after) == 0 and hours_since_upgrade >= 4.0:
    verdict = "PASS_NO_PANIC_4H"
elif len(panics_after) == 0:
    verdict = "WATCH_OK_SO_FAR"
elif len(panics_after) > 0:
    verdict = "FAIL_PANIC_AFTER_UPGRADE"
else:
    verdict = "UNKNOWN"

# 最近 30 分钟是否有 rst=4
recent_cut = now.timestamp() - 1800
recent_panics = [ts for ts, l in panics_after if ts.timestamp() >= recent_cut]

with open(out_path, "a") as f:
    f.write("=" * 60 + "\n")
    f.write(now.strftime("%Y-%m-%d %H:%M:%S") + " WATCH1388\n")
    f.write("uptime_since_upgrade_h=%.2f\n" % hours_since_upgrade)
    f.write("baseline_last_rst4=%s\n" % baseline.strftime("%Y-%m-%d %H:%M:%S"))
    f.write("upgrade_boot=%s\n" % upgrade_ts.strftime("%Y-%m-%d %H:%M:%S"))
    f.write("rst4_after_upgrade_count=%d\n" % len(panics_after))
    for ts, l in panics_after[-5:]:
        f.write("  PANIC %s %s\n" % (ts.strftime("%H:%M:%S"), l[:100]))
    f.write("rst4_last_30min=%d\n" % len(recent_panics))
    f.write("nfc_counts_after=%s\n" % dict(nfc_c))
    f.write("last_nfc=%s at %s\n" % (last_nfc, last_nfc_ts.strftime("%H:%M:%S") if last_nfc_ts else "-"))
    f.write("pn532_ready_after=%d\n" % len(pn_ready_after))
    f.write("setRetries_fail_after=%d\n" % setfail_after)
    f.write("nfc_dead_after=%d\n" % dead_after)
    if last_log:
        f.write("last_heartbeat=%s\n" % last_log[1][:160])
    f.write("verdict=%s\n" % verdict)
    if verdict == "PASS_NO_PANIC_4H":
        f.write("NOTE: log_ship 去 String 后升级满 4h 无 rst=4，第一刀初步有效\n")
    elif verdict == "WATCH_OK_SO_FAR":
        f.write("NOTE: 尚未满 4h，暂无 rst=4，继续观察\n")
    elif verdict == "FAIL_PANIC_AFTER_UPGRADE":
        f.write("NOTE: 升级后仍出现 rst=4，第一刀可能不足或另有根因\n")
    f.flush()

# 同时打一行摘要到 stdout（cron 可记 journal）
print("%s uptime=%.1fh panics=%d last_nfc=%s verdict=%s" % (
    now.strftime("%H:%M:%S"), hours_since_upgrade, len(panics_after), last_nfc, verdict))
PY
