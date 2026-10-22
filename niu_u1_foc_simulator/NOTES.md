# 小牛 U1 FOC 模拟器 — 项目笔记

> 板子: 天问 Pro TW002 (ESP32-C3-DevKitC-02 + CH340K)
> 串口: 485 → V35 ECU (8E1 9600),  USB CDC → 调试日志
> 上次更新: 2026-10-20 (移除 BLE 解锁 + WiFi/Web, OLED 改为只显示速度)

## 1. 硬件接线 (当前)

| 功能 | GPIO | 备注 |
|---|---|---|
| 485 TX | GPIO 7 | MAX485 DI (自动收发模块, 无 DE/RE 引脚) |
| 485 RX | GPIO 4 | MAX485 RO |
| OLED SDA | GPIO 5 | 板载 SSD1306 128×64, 地址 0x3C |
| OLED SCL | GPIO 6 |  |
| 霍尔轮速 | GPIO 3 | 1 磁钢, 轮周长 1.28m (16x2.50/64-305 实测), **INPUT_PULLUP + 下降沿中断**. ⚠️ **当前是写死模式, 传感器尚未装好** |
| USB CDC | native | platformio.ini 开了 `CDC_ON_BOOT=1` |

> ⚠️ **GPIO 9 是 ESP32-C3 的 BOOT 脚**, 不要当普通 GPIO 用. 刷机必须手动按住 BOOT.

> ⚠️ **霍尔传感器必须上拉**: A3144/SSR14Y 都是开集电极 (open-collector) 输出, 内部晶体管关断时输出脚是浮空的,
> 不上拉会读到随机噪声脉冲, 轮速全是乱跳. 代码里用 `pinMode(WHEEL_SPEED_PIN, INPUT_PULLUP)` 打开 ESP32 内部上拉
> (~45kΩ). 如果干扰大或线长, 建议外挂 4.7k~10k 上拉电阻. 磁铁经过时输出被拉低 → **下降沿中断 (FALLING)**.

## 2. 串口与日志

- **485 总线**: 必须在 `Serial1.begin(9600, SERIAL_8E1, 4, 7)`, **不是** 115200 8N1. 错配 1 位都收不到 V35 报文.
- **USB CDC**: `Serial.begin(115200)`, 仅作调试日志镜像, 不影响 485 总线.
- **minicom**: 默认不补 CR, 看到 "楼梯" 是正常的, 按 `Ctrl+A U` 打开 "Add Carriage Return" 即可.
- **日志刷屏**: `platformio.ini` 里 `CORE_DEBUG_LEVEL=3` (INFO), `log_v/log_d` 在编译期被替换成 `do {} while(0)`.
  关键: Arduino-ESP32 的 `log_v()` 走 `log_printf` 不走 `ESP_LOGV`, 所以 `esp_log_level_set()` 对它无效, 只能编译期关.

## 3. 485 协议 (V35 ECU 私有协议, DL/T 645 衍生)

帧格式: `68 [DEST_PAIR] 68 [FUNC] [LEN] [DATA+33...] [CHECKSUM] 16`

| 设备 | 地址对 (字节 1+2) |
|---|---|
| FOC 控制器 (本机) | `0x20 / 0xDF` |
| V35 中控 (主节点) | `0x40 / 0xBF` |
| 仪表 | `0x10 / 0xEF` |
| 电池 BMS | `0x31 / 0xCE` |

**关键点**:
- 字节 1+2 标识"这一帧说的是谁": 请求帧 = 目的设备, **应答帧 = 回话设备自己**.
  真车抓包: BMS 回 `31 CE`、仪表回 `10 EF`、FOC 回 `20 DF` —— FOC 应答一律用 `0x20 0xDF`.
  (代码里 `REPLY_WITH_ECU_ADDR` 想改成回 `0x40 0xBF` 时把它设成 1)
- 协议里**没有发送方地址字段** (因为 485 半双工总线隐式知道谁在说话)
- 数据区每个字节发送时 `+0x33` 混淆, 接收时 `-0x33` 反混淆
- 校验和 = 从字节 0 累加到 (不含) CS 字节, 取低 8 位 — **简单 sum & 0xFF**, 不是 CRC
- 轮询周期: V35 大约每 200ms 问一次 FOC

**已验证的示例帧** (Bilibili cv39475428 文章 7 条):
```
FOC read voltage     68 20 DF 68 01 01 38 09 16  (CS=0x09)
FOC read current     68 20 DF 68 01 01 3C 0D 16  (CS=0x0D)
ECU set master       68 40 BF 68 06 01 02 D8 16  (CS=0xD8)
Dashboard display    68 10 EF 68 04 0F ... 33 71 16  (CS=0x71)
... 全部用 sum & 0xFF 对得上
```

**我们 FOC 的应答** (2026-10 按真车抓包 + xzfn 的 ECU 源码对齐):

状态查询 `0x02 05 08` → 12 字节 (`0x05 09` → 14 字节, 多 2 字节 `00 02`):
```
68 20 DF 68 82 0C [30 26 3C 01 00 19 00 00 00 SS 08 00 → +0x33] [CS] 16
                                 └flag_0  保留 保留 保留 保留 保留 │ └SS=车速协议值(0-31) [9]  └flag_1(0x08=驻车) [10]
```
字段定义来自 `niu-ecu/User/niu_series_u.h` 的 `struct PayloadFOCInfoResponse`
(长度必须正好 12, 否则 ECU 判定通信失败并报错误码 190)。ECU 换算: 仪表速度 = 车速值×12/25。

控制帧 `0x01` 的子命令 (原厂 FOC 的固定应答):
| 子命令 | 应答 | 说明 |
|---|---|---|
| `0x01` | `0x81` + 1 字节 `00` | 状态标志 |
| `0x02` | `0x81` + 16 字节 ASCII | 版本串 `U1E02V11U1E02V11` |
| `0x03` | `0x81` + 16 字节 ASCII | MU 编号 `MU00001B00000001` (兼作解锁命令) |
| `0x04` | `0x81` + 1 字节 `11` | 固件版本 |

系统配置 `0x06` → `0x86` + **空数据** (原厂就是空 ACK)。

## 4. 刷机流程

> 板子的 CH340K 没把 GPIO9 (BOOT) 接到 DTR, 软件复位进不了下载模式, **必须手动按 BOOT**

```bash
# 1. 拔 USB
# 2. 按住 BOOT 不放
# 3. 插回 USB (设备进入下载模式)
# 4. 立刻跑 (脚本会持续轮询 5 次连接)
bash /tmp/flash_after_boot.sh
# 5. 看到 "Writing at 0x..." 进度 → 松开 BOOT
# 6. "Hard resetting via RTS pin..." → 完成
```

脚本: `/tmp/flash_after_boot.sh` (主用), `/tmp/flash_now.sh` (备, 30s 轮询)

`platformio.ini` 里 `board_build.flash_size = 4MB` (TW002 实际是 4MB flash, 不是 8MB).

## 5. 当前 setup() 顺序 (2026-10-20 版, 已稳定)

```
1. Serial.begin(115200)            # USB CDC
2. Wire.begin(5, 6) + setClock     # ← 关键! 必须最早, 防止 u8g2 首刷时 Wire lock 还没创建
3. Serial1.begin(9600, 8E1, 4, 7)  # 485 总线 (RX=4, TX=7)
4. esp_log_level_set(*)            # 静音 ESP-IDF
5. simulator.begin()               # 霍尔 ISR (INPUT_PULLUP + FALLING)
6. u8g2.begin()                    # OLED
```

去掉了原来的 `initBLE()` 和 `webServer->begin()` 两步 — BLE 解锁功能实现不了, 连带的
WiFi AP / Web 扫描页面 (web_server.h / web_pages.h / web_config.h) 已整体删除.
省下的不只是 Flash: 原来 `FALLBACK_AP_MODE == 0` 时 Web 服务器会阻塞等 WiFi 连接最多 20 秒,
现在上电直接进主循环, 485 响应更快.

资源占用变化: Flash 37.1% (1508362 B) → **7.5% (306106 B)**, RAM 19.5% (63820 B) → **4.9% (16152 B)**.

## 5.1 轮速来源开关 (当前: 写死)

```cpp
#define USE_HALL_SPEED    0      // 0=写死车速, 1=霍尔实测
#define HARDCODE_SPEED_DM 170    // 写死车速 km/h × 10 (170 = 17.0 km/h)
```

霍尔磁钢盘还没装好, 所以先用写死值把 485 通信链路和仪表显示调通.
**写死 17.0 km/h → 协议值 31** (17 超出协议对照表范围, 31 是封顶值, 仪表显示约 17 km/h).

磁钢盘装好后: 把 `USE_HALL_SPEED` 改成 `1` 重新编译烧录即可, 速度映射逻辑完全一样.
写死模式下 `wheel_pulse_count` 仍在正常计数 (ISR 照常挂载), 只是不用它算速度,
所以装好霍尔后不用改任何其他代码.

## 6. 已知问题 / 坑

| 问题 | 状态 | 备注 |
|---|---|---|
| 485 收不到任何报文 | ✅ 修 | 波特率 115200→9600, 8N1→8E1 |
| 回包地址错 (V35 拒收) | 🔄 2026-10 改回 | 真车 FOC 应答带的是**自己的**地址对 `20 DF` (BMS 回 `31 CE`、仪表回 `10 EF`), 之前用 `40 BF` 与真车不符, 已改回; 若 V35 实测要求 `40/BF`, 把 `REPLY_WITH_ECU_ADDR` 设 1 |
| Wire.cpp 开机错误刷屏 | ✅ 修 | Wire.begin 提前到 setup 顶部 |
| ESP-IDF log 刷屏 | ✅ 修 | `CORE_DEBUG_LEVEL=3` (编译期关 log_v/log_d) |
| 霍尔轮速乱跳 | ✅ 修 | 原来 `INPUT` 无上拉 → 改 `INPUT_PULLUP` |
| 霍尔磁钢盘安装 | ⚠️ 进行中 | 磁钢已改成 1 个, 周长实测 1.28m; 但 `USE_HALL_SPEED` 还是 0 (写死 17 km/h), 装好传感器后改成 1 |
| 状态帧长度/字段错位 | ✅ 修 | 原厂 `0x02 05 08` 应答是 **12 字节**, 车速在**第 9 字节** (协议值 0-31), 之前回 14 字节且把速度放在 [3][4], ECU 读不到车速还会报错误码 190 |
| GPIO2 占用 | ✅ 修 | strapping pin, 485 RX 挪到 GPIO4, TX 挪到 GPIO7 |
| BLE 防丢器自动解锁 | ❌ 已放弃 | 红米 Watch 3 广播窗口太短 + iPhone 侧无法配合, 实现不了, 已移除 |
| NFC (PN532) 无钥匙解锁 | ❌ 已移除 | 2026-10-22 删除驱动代码/接线文档/库依赖, I2C 只留板载 OLED |
| 485 FOC 解锁 CRC16 (poly 0x7085) | ❌ 未实现 | 真正的 FOC 解锁需要 CRC16 算 token, 现在只处理 0x01 unlock 命令立即返回成功 |

## 7. 调试技巧

- **看 485 收**: loop() 里 `[485←]` 逐字节回显, 帧尾 0x16 换行
- **看 485 发**: `sendFrame()` 里 `[485→]` 回显整帧
- **看轮速**: `foc_status.wheel_rpm` / `wheel_speed_dm` (十分位整数), OLED 上直接显示
- **看状态**: `[状态] 已解锁 | 轮速:xxRPM | x.xkm/h` 每 3 秒一次
- **想看 485 完整原始帧**: `od -A x -t x1 /dev/ttyUSB0` 旁路串口工具 (需要 USB-485 转换器)

## 8. 关键文件位置

| 文件 | 作用 |
|---|---|
| `src/niu_u1_foc_simulator.ino` | 主程序, 唯一 .ino |
| `platformio.ini` | build flags: CORE_DEBUG_LEVEL=3, flash_size=4MB, CDC_ON_BOOT=1 |
| `partitions.csv` | 4MB flash 分区表 |
| `/tmp/flash_after_boot.sh` | 主用刷机脚本 |

## 9. 引用资料

- [B 站: 小牛 V35 中控专用协议总结](https://www.bilibili.com/read/cv39475428) — 帧格式 / 校验和 / 7 条示例帧
- [GitHub: xzfn/scooter-repair](https://github.com/xzfn/scooter-repair) — 用 CH32V003 替代 V35 ECU, 含 FOC 解锁 CRC16 (poly 0x7085)
- [SMZDM: 开源救活 11 年老小牛](https://post.smzdm.com/p/a3mdqg4r/) — 海外/公司网络可能打不开, 国内可访问
