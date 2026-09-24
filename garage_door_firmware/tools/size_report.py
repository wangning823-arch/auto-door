#!/usr/bin/env python3
"""Analyze firmware.elf + firmware.map for flash size attribution."""
from __future__ import annotations

import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "esp32dev"
ELF = BUILD / "firmware.elf"
MAP = BUILD / "firmware.map"
BIN = BUILD / "firmware.bin"


def elf_sections(path: Path) -> list[tuple[int, str, int, int]]:
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF", "not ELF"
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize = struct.unpack_from("<H", data, 46)[0]
    e_shnum = struct.unpack_from("<H", data, 48)[0]
    e_shstrndx = struct.unpack_from("<H", data, 50)[0]
    shstr_off = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 16)[0]
    shstr_size = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 20)[0]
    shstr = data[shstr_off : shstr_off + shstr_size]
    rows = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        name_off, _sh_type, sh_flags, sh_addr, _off, sh_size = struct.unpack_from(
            "<IIIIII", data, base
        )
        end = shstr.find(b"\x00", name_off)
        name = shstr[name_off:end].decode("ascii", "replace")
        if sh_size and (sh_flags & 0x2):
            rows.append((sh_size, name, sh_addr, sh_flags))
    rows.sort(reverse=True)
    return rows


def map_contrib(text: str):
    contrib = defaultdict(lambda: defaultdict(int))
    sec = None
    # also track path like libxxx.a(file.o)
    for line in text.splitlines():
        m = re.match(r"^(\.[A-Za-z0-9_.]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s*$", line)
        if m:
            sec = m.group(1)
            continue
        m = re.match(
            r"^\s+(\.[A-Za-z0-9_.$]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+\.o)\b",
            line,
        )
        if not m:
            continue
        size = int(m.group(2), 16)
        path = m.group(3).strip().replace("\\", "/")
        # normalize object identity
        if "(" in path and path.endswith(")"):
            lib, obj = path.split("(", 1)
            obj = obj[:-1]
            base = f"{lib.split('/')[-1]}({obj.split('/')[-1]})"
        else:
            base = path.split("/")[-1]
        kind_name = sec or m.group(1)
        kl = kind_name.lower()
        if "rodata" in kl:
            k = "rodata"
        elif "text" in kl or "iram" in kl:
            k = "text"
        elif "data" in kl or "dram" in kl:
            k = "data"
        else:
            k = "other"
        contrib[base][k] += size
        contrib[base]["total"] += size
    return contrib


CATS = [
    (r"BluetoothSerial|bt_app|bta_|bluedroid|libbt\.|libbtdm|libnimble|BLEDevice|BLE[A-Z]|libBLE", "BT host/controller"),
    (r"WiFiClientSecure|ssl_client|libssl|libmbed|libwpa|crypto", "TLS/crypto"),
    (r"WiFiGeneric|WiFiSTA|WiFiAP|WiFiScan|WiFiClient|WiFiUdp|WiFiServer|libWiFi|esp_wifi|libnet80211|libphy|libpp|librtc_clk|libesp_coex|libesp_common|libesp_phy|libesp_wifi|libwpa2|libesp_netif|libesp_event|libtcpip_adapter|libesp_eth|libesp_netif", "WiFi/IP stack"),
    (r"lwip|liblwip", "lwIP"),
    (r"WebServer|Parsing", "WebServer"),
    (r"ArduinoOTA|Updater|libUpdate", "OTA"),
    (r"Preferences|nvs_|libnvs", "NVS"),
    (r"ESPmDNS|mdns", "mDNS"),
    (r"Wire\b|libWire", "Wire/I2C"),
    (r"Adafruit|PN532", "PN532"),
    (r"HardwareSerial|WString|Esp|esp32-hal|libFramework|Print|Stream|USBCore|Tone|EspSoftwareSerial", "Arduino core"),
    (r"app|web_portal|main\.cpp|ble_|rf_|nfc_|door_|remote_|log_ship|status_report|config_store|default_rf", "app src"),
    (r"esp_system|esp_hw_support|esp_rom|esp_common|esp_hw|soc\.|hal\.|spi_flash|esp_partition|esp_ota|bootloader|esp_timer|freertos|heap|esp_coex|newlib|libpthread|libgcc|libstdc|libnosys|esp_rom", "IDF/system"),
]


def categorize(name: str) -> str:
    for pat, lab in CATS:
        if re.search(pat, name, re.I):
            return lab
    return "other"


def main() -> None:
    bin_size = BIN.stat().st_size if BIN.exists() else 0
    print(f"BIN_SIZE={bin_size}")
    if ELF.exists():
        rows = elf_sections(ELF)
        print("\n== Top ALLOC sections (ELF) ==")
        for sz, name, addr, _fl in rows[:30]:
            print(f"{sz:10d}  {name:42s} addr=0x{addr:08x}")
        print("ALLOC total", sum(r[0] for r in rows))
    else:
        print("ELF missing", ELF)

    if not MAP.exists():
        print("MAP missing")
        return
    text = MAP.read_text(errors="replace")
    contrib = map_contrib(text)
    items = sorted(contrib.items(), key=lambda kv: kv[1]["total"], reverse=True)
    print("\n== Top objects by map contribution ==")
    for name, d in items[:50]:
        print(
            f"{d['total']:8d}  text={d['text']:7d} rodata={d['rodata']:7d} "
            f"data={d['data']:6d}  {name}"
        )
    total = sum(d["total"] for _, d in items)
    print("SUM", total)

    libsum = defaultdict(int)
    for name, d in contrib.items():
        libsum[categorize(name)] += d["total"]
    print("\n== Category totals ==")
    for k, v in sorted(libsum.items(), key=lambda kv: -kv[1]):
        print(f"{v:8d}  {k}")

    # top rodata (strings)
    ro = sorted(
        ((n, d["rodata"]) for n, d in contrib.items() if d["rodata"]),
        key=lambda x: -x[1],
    )
    print("\n== Top rodata (strings/const) ==")
    for n, s in ro[:25]:
        print(f"{s:8d}  {n}")


if __name__ == "__main__":
    main()
