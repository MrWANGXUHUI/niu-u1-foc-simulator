# 小牛U1 FOC模拟器 — 开发记录

## 项目概述
基于 ESP32-C3 开发的小牛 U1 电动车 FOC 控制器模拟器，替代已损坏的原车 FOC 控制器，
响应 V35 中控的 485 轮询，让整车（中控 / 仪表 / 电池）认为 FOC 工作正常。

使用 PlatformIO + Arduino 框架开发。

> 当前接线表、协议细节、已知问题见 **`niu_u1_foc_simulator/NOTES.md`**（活文档，以它为准）。
> 本文只记录开发方式和历史决策。

---

## 硬件配置

| 组件 | 型号/参数 |
|------|-----------|
| 主控 | 天问 Pro TW002 (ESP32-C3-DevKitC-02, **4MB Flash**) |
| 显示 | 0.96寸 OLED (SSD1306, 128×64, I2C GPIO5/6, 板载) |
| **485 通信** | **Serial1 (9600 8E1, GPIO7=TX / GPIO4=RX)** 连接 V35 ECU (地址 0x40) |
| **调试日志** | **USB CDC (Serial, 115200 8N1)** — 仅 ASCII 文本，独立通道 |
| 轮速 | 霍尔传感器 A3144/SSR14Y (GPIO3, 1 磁钢) + 轮胎 16x2.50/64-305 (周长 1.28m) |

> **串口完全隔离：** `platformio.ini` 里 `CDC_ON_BOOT=1` 启用 USB CDC。代码里所有 `Serial.printf` 走 USB，
> 所有 `Serial1.*` 只用于 485 帧收发，互不干扰。
>
> **Serial1 引脚为何不选 GPIO20/21：** ESP-IDF 启动日志系统会把 UART0 初始化在 GPIO20/21，
> Serial1 (UART1) 再去抢同一对引脚会导致 driver 死锁。
> GPIO2/8/9 是 strapping pin，GPIO11-17/18/19/20/21 分别被 flash / USB / UART0 占用，
> 所以 485 只用安全引脚 **GPIO7 (TX→MAX485 DI)** 和 **GPIO4 (RX←MAX485 RO)**。

### 引脚选型速查
ESP32-C3 上**完全安全**的 GPIO 只有 **3、4、5、6、7、10** 六个。
本项目 5 个外设引脚全部落在安全区，没有一个碰到 strapping / flash / USB。

---

## 功能模块

### 1. 485 帧解析
- 帧格式：`68 [地址对] 68 [功能码] [字节数] [数据+0x33] [校验和] 16`
- 校验和 = 从帧头累加到 CS 前一字节，取低 8 位（**sum & 0xFF，不是 CRC**）
- 字节 1+2 永远是"目的设备对"，不是 Modbus 站号；协议里没有发送方地址字段

### 2. V35 ECU 协议握手
整车 485 主节点是 **V35 ECU（地址 0x40）**，不是仪表。模拟器作为从节点 **0x20** 响应：

| FUNC | 含义 | 模拟器响应 |
|---|---|---|
| `0x01` | 控制命令 | 子命令 `0x02`→版本串 `U1E02V11U1E02V11`、`0x03`→MU 编号 `MU00001B00000001`（兼解锁）、`0x04`→`0x11` |
| `0x02` | 状态查询 | `05 08`→12 字节状态帧（车速协议值在第 9 字节）；`05 09`→14 字节 |
| `0x04` | 写数据 | ACK `0xCD` |
| `0x05` | 设置参数 | ACK `0xCE` |
| `0x06` | 系统配置（主/从模式） | 空数据 ACK（原厂即如此） |

> **应答帧地址对是「回话设备自己」**：真车抓包里 BMS 回 `31 CE`、仪表回 `10 EF`、FOC 回 `20 DF`，
> 所以本模拟器应答固定用 `0x20 / 0xDF`（`REPLY_WITH_ECU_ADDR` 置 1 可改回 `0x40 / 0xBF`）。

所有 V35 命令都通过 `Serial.printf("[485←] ...")` / `"[485→] ..."` 输出，方便抓包调试。

### 3. 霍尔轮速测量
- GPIO3，`INPUT_PULLUP` + 下降沿中断
- **霍尔必须上拉**：A3144/SSR14Y 是开集电极输出，不上拉读到的是噪声脉冲，轮速会乱跳
- ISR 加 1ms 消抖（1 磁钢在 30km/h 下脉冲间隔约 153ms，不会漏计）
- **当前是写死模式**：`USE_HALL_SPEED 0` → 写死 17.0 km/h（协议值 31），磁钢盘装好后改成 1

### 4. OLED 显示
只显示速度：大字 `km/h` + 底部 RPM / 错误码，每 200ms 刷新。

---

## 已移除的功能

### BLE 防丢器接近解锁（2026-10-20 移除）
原设想用 BLE 防丢器 RSSI 做无钥匙解锁，已放弃。

**放弃原因**：红米 Watch 3 广播窗口极短（1-2 秒）抓不稳；iPhone 不主动广播 BLE、也无法自建 iOS App；
依赖的 WiFi AP + Web 扫描页面（`web_config.h` / `web_pages.h` / `web_server.h`）一并删除。

**替代**：V35 中控的 `0x01` unlock 命令直接应答成功，挡位解锁由中控自己判断。

> 2026-10-22：NFC（PN532）方案也一并移除 —— 驱动代码、接线文档、库依赖全部删掉，I2C 只留板载 OLED。

---

## 分区方案

```
nvs,      data, nvs,     0x9000,  0x5000,
ota,      data, ota,     0xe000,  0x2000,
app0,     app,  factory, 0x10000, 0x3E0000,  // 4MB app分区
coredump, data, coredump,0x3F0000,0x10000,
```

固件当前约 300KB（移除 BLE/Web 后从 1.5MB 降下来），4MB app 分区绰绰有余。

---

## 文件结构

```
niu/
├── README.md
├── DEVELOPMENT.md              # 本文件
├── docs/                       # 设计背景 / 协议分析 / 视频提示词
├── diagrams/                   # 接线拓扑图
└── niu_u1_foc_simulator/
    ├── platformio.ini
    ├── partitions.csv
    ├── README.md
    ├── NOTES.md                # 活文档: 接线表 / 协议 / 已知问题
    └── src/
        └── niu_u1_foc_simulator.ino  # 主程序 (485协议 + 霍尔轮速 + OLED)
```

---

## 烧录命令

TW002 的 CH340K 没把 GPIO9 (BOOT) 接到 DTR，软件复位进不了下载模式，**必须手动按 BOOT**：

```bash
# 1. 拔 USB
# 2. 按住 BOOT 不放
# 3. 插回 USB (进入下载模式)
# 4. 立刻跑
bash /tmp/flash_after_boot.sh
# 5. 看到 "Writing at 0x..." 进度 → 松开 BOOT
```

编译（不烧录）：

```bash
cd /home/wangxuhui/Documents/niu/niu_u1_foc_simulator
pio run
```

串口：`/dev/ttyUSB0`，波特率 **115200**（USB CDC 调试日志，不是 485）。
