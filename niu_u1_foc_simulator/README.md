# 小牛U1 FOC控制器模拟器

基于ESP32-C3的小牛U1电动车FOC电机控制器模拟器，实现损坏FOC控制器的功能替代，并添加霍尔轮速测速和OLED速度显示。

## 功能特性

- **FOC协议模拟** - 完整兼容小牛U1 485总线协议，响应V35中控轮询
- **霍尔轮速测量** - 实时测量轮速，OLED显示车速
- **电量统计** - 精确计算电池能耗(Wh/km)
- **原厂身份模拟** - 应答里带原厂 FOC 的 MU 编号 `MU00001B00000001`、版本串 `U1E02V11U1E02V11`，V35 校验通过

> 原计划的「BLE防丢器接近解锁」已放弃（红米手环广播窗口太短抓不稳，且iPhone无法配合），代码中已移除。
> NFC（PN532）无钥匙解锁方案也已于 2026-10-22 移除（驱动代码 / 接线文档 / 库依赖全部删除，I2C 只留板载 OLED）。

## 硬件清单

| 配件 | 型号/规格 | 数量 | 用途 |
|------|-----------|------|------|
| 主控 | 天问Pro TW002 (ESP32-C3-DevKitC-02) | 1 | 系统控制 |
| 通信 | MAX485模块(自动收发型) | 1 | RS485总线通信 |
| 轮速 | 霍尔传感器(SSR14Y/A3144) | 1 | 轮速测量 |
| 磁钢 | 单个磁铁（1 极） | 1 | 配合霍尔传感器 |
| 电源 | DC-DC降压模块 48V→5V | 1 | 供电 |

> 轮胎：**16x2.50/64-305**，实测转一圈 **1.28 米**（不是标称值）。
> 固件里对应 `MAGNET_COUNT = 1`、`WHEEL_CIRCUMFERENCE_M = 1.28`。

## 协议说明

### 485总线配置
- 波特率: **9600**
- 数据位: **8E1** (8数据位 + 偶校验 + 1停止位)
- 校验和: 从帧头累加到CS前一个字节，取低8位 (sum & 0xFF，**不是** CRC)
- 帧格式: `68 [地址] [地址反码] 68 [功能码] [字节数] [数据+0x33] [校验和] 16`

### 设备地址
| 设备 | 地址 | 备注 |
|------|------|------|
| FOC电机控制器 | 0x20 | 本模拟器 |
| 电池BMS | 0x31 | |
| 仪表 | 0x10 | |
| 中控 V35 ECU | 0x40 | 485 主节点 |

> **应答帧的地址对是「回话设备自己」**：真车抓包里 BMS 回 `31 CE`、仪表回 `10 EF`、FOC 回 `20 DF`。
> 所以本模拟器的应答固定用 `0x20 / 0xDF`（代码里 `REPLY_WITH_ECU_ADDR` 置 1 可改回 `0x40 / 0xBF`）。

### 关键命令
| 命令 | 说明 |
|------|------|
| 0x01 0x03 | 解锁挡位（应答 0x81 + MU 编号 16 字节） |
| 0x02 0x05 0x08 | 查询FOC状态（应答 0x82 + 12 字节，车速在第 9 字节） |
| 0x02 0x05 0x09 | 查询FOC状态（应答 0x82 + 14 字节） |
| 0x01 0x02 / 0x04 | 版本串 / 固件版本 |
| 0x06 | 系统配置（空数据 ACK，与原厂一致） |
| 0x82 | 响应标志(查询响应) |

## 软件架构

```
niu_u1_foc_simulator/
├── platformio.ini
├── partitions.csv
├── README.md
├── NOTES.md                    # 活文档: 接线表 / 协议 / 已知问题
└── src/
    └── niu_u1_foc_simulator.ino  # 主程序 (485协议 + 霍尔轮速 + OLED)

docs/
├── design_background.md      # 开发笔记(背景、需求、硬件选型)
└── protocol_analysis.md      # 协议分析(帧格式、命令详解)
```

## 快速开始

1. 硬件接线 (见下文)
2. 安装ESP32开发板驱动
3. 烧录主程序
4. 磁铁装到轮子上, 霍尔传感器对准磁铁
5. 测试通信

## 接线图

```
ESP32 GPIO7 ──→ MAX485 DI   (485 TX)
ESP32 GPIO4 ──→ MAX485 RO   (485 RX)
ESP32 GPIO5 ──→ OLED SDA    (板载 I2C)
ESP32 GPIO6 ──→ OLED SCL    (板载 I2C)
ESP32 GPIO3 ──→ 霍尔传感器 OUT

MAX485 A/B ──→ 485总线 A/B
霍尔传感器 VCC/GND ──→ 5V/GND
```

> MAX485 用的是**自动收发**小板，板内自带 RC 延时 + 三极管，不暴露 DE/RE 引脚。
> 485 通信参数：**9600 8E1**（不是 115200 8N1）。

> ⚠️ **霍尔传感器必须上拉**：A3144/SSR14Y 是开集电极输出，不上拉会读到噪声脉冲导致轮速乱跳。
> 代码用 `INPUT_PULLUP` 开 ESP32 内部上拉，线长或干扰大时建议外挂 4.7k~10k 上拉电阻。

> ⚠️ **GPIO9 是 BOOT 键**，不要当普通 GPIO 用；TW002 刷机必须手动按住 BOOT。

## 功耗

| 项目 | 值 |
|------|-----|
| ESP32 + 外设 | ~0.5W |
| 48V24Ah电池续航影响 | 可忽略 |

## 参考资料

### 本项目文档

| 文档 | 地址 | 说明 |
|------|------|------|
| 接线与 GPIO 完整文档（飞书） | https://fcnm1ydb2e5g.feishu.cn/docx/XwprdaKRmoQgUixAL2ncKcq4nzc | 接线图、GPIO 定义、协议帧、异常排查（对外版） |
| 活文档 | [NOTES.md](NOTES.md) | 接线表 / 协议 / 已知问题，以它为准 |
| 开发记录 | [DEVELOPMENT.md](../DEVELOPMENT.md) | 开发方式与历史决策 |
| 协议分析 | [docs/protocol_analysis.md](../docs/protocol_analysis.md) | 帧格式、命令详解 |
| 开发背景 | [docs/design_background.md](../docs/design_background.md) | 需求分析、硬件选型 |

### 开源项目 / 逆向资料

| 来源 | 地址 | 用途 |
|------|------|------|
| xzfn / scooter-repair | https://github.com/xzfn/scooter-repair | 真车 485 抓包日志 + 可用的 V35 ECU 替代固件。本模拟器的应答帧格式（MU 编号 `MU00001B00000001`、版本串 `U1E02V11U1E02V11`、12/14 字节状态帧字段布局）全部对齐它；含 FOC 解锁 CRC16（poly 0x7085）算法 |
| Jonesyme / niu-bms-readout | https://github.com/Jonesyme/niu-bms-readout | Python 实现的 BMS 上位机协议 |
| B 站 cv39475428 | https://www.bilibili.com/read/cv39475428 | 《小牛 V35 中控专用协议总结》—— 帧格式、校验和、示例帧 |
| 小牛电池协议逆向 | https://zhuanlan.zhihu.com/p/419857440 | 电池侧协议 |
| SMZDM 开源救活老小牛 | https://post.smzdm.com/p/a3mdqg4r/ | 整体方案参考 |

## License

MIT
