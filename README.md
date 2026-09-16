# 车库门智能控制器 P0 固件

对应商品方案 `商品方案_学习型智能车库门控制器.md` 的 P0：

- **F0** 防砸车：`TRANSIT` / `T_clear` / 关前 abort
- **F1a** 仅蓝牙**渐近**（`GRADUAL_IN`）自动开
- **F2a** 仅蓝牙**渐离**（`GRADUAL_OUT`）+ 清空后自动关
- **突变**（`SUDDEN_APPEAR` / `SUDDEN_LOSS`）**不动作**
- **F3** 手动开且无车不自动关
- TRIG 输入：米家插座路径
- 门磁、继电器脉冲、串口调试

## 接线（默认，可在 `config.h` 改）

| 功能 | GPIO |
|---|---|
| 继电器 | 26 |
| 门磁（上拉，闭合=关） | 27 |
| TRIG 米家（上拉，对地触发） | 25 |
| 学习键（板载 BOOT） | 0 |
| 状态 LED | 2 |

## 编译烧录（PlatformIO）

```powershell
cd D:\mimo\车库门自动化\garage_door_firmware
python -m platformio run -t upload
python -m platformio device monitor
```

先在 `src/config.h` 改 `CAR_BT_MAC` 为你车机蓝牙 MAC，或运行时串口执行：

```
mac AA:BB:CC:DD:EE:FF
status
```

## 原固件备份

`../firmware_backup/`：已保存 `partitions.bin`、`nvs.bin`。  
整片 4MB 在高波特率下 USB 不稳未能完整读出；原片为 Espressif ESP-IDF AT/IoT（`Wroom32:1.1.2`），需要时可再刷官方 AT 镜像。

**烧录本固件会覆盖 OTA app 分区，原 AT 固件将不可用（除非重新刷 AT）。**
