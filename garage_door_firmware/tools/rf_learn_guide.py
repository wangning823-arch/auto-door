# -*- coding: utf-8 -*-
"""非交互引导：每步倒计时后发 rflearn，等学习成功（超时30s）"""
import time
import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


KEYS = [
    (0, "上/开"),
    (1, "下/关"),
    (2, "暂停"),
]


def drain(ser, seconds=0.3):
    end = time.time() + seconds
    while time.time() < end:
        if ser.read(2048):
            end = time.time() + 0.1
        else:
            time.sleep(0.05)


def read_until(ser, keywords, timeout=30.0):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = ser.read(512)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if text:
                    print(text)
                for kw in keywords:
                    if kw in text:
                        drain(ser, 0.5)
                        return text
        else:
            time.sleep(0.05)
    return ""


def main():
    port = pick_port()
    print(f"串口 {port}")
    ser = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    time.sleep(0.3)
    ser.reset_input_buffer()
    print("等待固件就绪 3s...")
    time.sleep(3)
    drain(ser, 1.0)

    for idx, name in KEYS:
        print("\n" + "=" * 52)
        print(f"按键 {idx}（{name}）")
        print("请把遥控器对准接收天线 5~10cm，准备短按...")
        for sec in (3, 2, 1):
            print(f"  {sec}...")
            time.sleep(1)
        print(f"发送 rflearn {idx} —— 现在就按遥控【{name}】！")
        ser.reset_input_buffer()
        ser.write(f"rflearn {idx}\n".encode())
        ser.flush()
        t0 = time.time()
        result = read_until(
            ser,
            ["学习成功", "抓包失败", "提不出", "波形不像固定码", "保存 NVS 失败"],
            timeout=35.0,
        )
        # 固件最短监听约 2s+预热；过早回显视为噪音
        if "学习成功" in result and (time.time() - t0) < 1.2:
            print(">>> 忽略过早的学习成功（疑似噪音），请重试")
            result = ""
        if "学习成功" in result:
            print(f">>> 按键 {idx} 成功")
        else:
            print(f">>> 按键 {idx} 失败/超时，请再跑一次或串口 rflearn {idx}")
            # 给用户喘息
            time.sleep(2)

    print("\n" + "=" * 52)
    print("发送 rfkeys 查看状态...")
    time.sleep(1)
    ser.write(b"\nrfkeys\n")
    time.sleep(1)
    drain(ser, 2.0)
    print("可用: rfplay 0  rfplay 1  rfplay 2")
    ser.close()


if __name__ == "__main__":
    main()
