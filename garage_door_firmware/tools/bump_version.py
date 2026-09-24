# PlatformIO pre-script: 生成 src/fw_version.h
# 版本 = 0.2.YYYYMMDDHHmm，时间取 src/ 下源码最新 mtime（改代码才升版本）
import os
import time

Import("env")  # noqa: F821  # PlatformIO SCons

MAJOR = "0.2"
SRC_DIR = os.path.join(env["PROJECT_DIR"], "src")  # noqa: F821
OUT = os.path.join(SRC_DIR, "fw_version.h")


def latest_src_mtime():
    newest = 0.0
    for root, _dirs, files in os.walk(SRC_DIR):
        for name in files:
            if not name.endswith((".c", ".cpp", ".h", ".hpp", ".ino")):
                continue
            # 生成头文件本身不算“改代码”
            if os.path.abspath(os.path.join(root, name)) == os.path.abspath(OUT):
                continue
            try:
                newest = max(newest, os.path.getmtime(os.path.join(root, name)))
            except OSError:
                pass
    return newest


def write_version():
    ts = time.strftime("%Y%m%d%H%M", time.localtime(latest_src_mtime() or time.time()))
    ver = f"{MAJOR}.{ts}"
    content = (
        "#pragma once\n"
        "// 由 tools/bump_version.py 在编译前生成，勿手改\n"
        f'#define FW_VERSION "{ver}"\n'
        f"#define FW_BUILD_TS {ts}\n"
    )
    old = ""
    if os.path.isfile(OUT):
        with open(OUT, "r", encoding="utf-8") as f:
            old = f.read()
    if old != content:
        with open(OUT, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"[version] FW_VERSION={ver}")


write_version()
