# -*- coding: utf-8 -*-
"""6帧滚码结构试探：XOR/固定位/计数器"""
import json
from pathlib import Path

caps = json.loads(
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_173303.json").read_text(
        encoding="utf-8"
    )
)


def split_parts(p):
    for i, v in enumerate(p):
        if 3500 <= v <= 4500:
            return p[:i], p[i + 1 :]
    return [], p


def body(p):
    return [v for v in p if v >= 150]


def bits_pwm(body, phase=0, lo=600):
    bits = []
    for i in range(phase, len(body), 2):
        v = body[i]
        if v > 1500:
            bits.append("?")
        else:
            bits.append("1" if v >= lo else "0")
    return "".join(bits)


bodies = [body(split_parts(c["pulses"])[1]) for c in caps]

# 两种相位
for phase in (0, 1):
    seqs = [bits_pwm(b, phase) for b in bodies]
    # 去掉 ?
    print(f"\n===== phase={phase} =====")
    print("长度:", [len(s) for s in seqs])
    for i, s in enumerate(seqs):
        print(f"#{i+1}: {s}")

    # 固定 bit：六帧同一位置相同（忽略 ?）
    n = min(len(s) for s in seqs)
    fixed_0 = fixed_1 = mixed = 0
    mixed_pos = []
    for pos in range(n):
        vals = [s[pos] for s in seqs if s[pos] in "01"]
        if len(vals) < 4:
            continue
        if all(v == vals[0] for v in vals):
            if vals[0] == "0":
                fixed_0 += 1
            else:
                fixed_1 += 1
        else:
            mixed += 1
            mixed_pos.append(pos)
    print(f"固定0={fixed_0} 固定1={fixed_1} 变化位={mixed}")
    print(f"变化位位置(前40): {mixed_pos[:40]}")

    # 相邻帧 XOR（同相位）
    def xor_bits(a, b):
        n = min(len(a), len(b))
        return "".join(
            "x" if (a[i] not in "01" or b[i] not in "01") else str(int(a[i]) ^ int(b[i]))
            for i in range(n)
        )

    print("相邻 XOR（x=无效）:")
    for i in range(len(seqs) - 1):
        x = xor_bits(seqs[i], seqs[i + 1])
        ones = x.count("1")
        valid = x.count("0") + x.count("1")
        print(f"  {i+1}^{i+1+1}: ones={ones}/{valid}  {x}")

    # 汉明距离
    print("两两汉明距离:")
    for i in range(len(seqs)):
        for j in range(i + 1, len(seqs)):
            x = xor_bits(seqs[i], seqs[j])
            ones = x.count("1")
            valid = x.count("0") + ones
            print(f"  {i+1} vs {j+1}: {ones}/{valid}")

# 尝试把 even/odd 高低位组合成更规整的 bit 对
print("\n===== 尝试：数据段前 20 个脉冲原始值 =====")
for i, b in enumerate(bodies):
    print(f"#{i+1}: {b[:24]}")

# 检查是否像 KeeLoq：通常 64 数据位 + 前导
# 检查简单计数器：低 8~16 bit 是否递增
print("\n===== 若把 even 相位当数据，低 8 bit 十进制 =====")
for phase in (0, 1):
    print(f"phase {phase}:")
    for i, b in enumerate(bodies):
        s = bits_pwm(b, phase)
        if len(s) >= 8:
            tail = s[-8:]
            if "01" <= tail.replace("?", "0") <= "11" and set(tail) <= set("01"):
                print(f"  #{i+1} tail8={tail} val={int(tail, 2)}")
            else:
                print(f"  #{i+1} tail8={tail}")
