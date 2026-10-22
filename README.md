# 小牛 U1 FOC 模拟器

用 ESP32-C3 替代小牛 U1 电动车损坏的原厂 FOC 电机控制器，在 485 总线上应答 V35 中控的轮询，
让中控 / 仪表 / 电池认为 FOC 工作正常，同时用霍尔传感器测轮速并在 OLED 上显示车速。

> 硬件接线与 GPIO 定义的**对外完整文档**在飞书：
> https://fcnm1ydb2e5g.feishu.cn/docx/XwprdaKRmoQgUixAL2ncKcq4nzc

## 硬件

| 组件 | 型号 / 参数 |
|------|------------|
| 主控 | 天问 Pro TW002（ESP32-C3-DevKitC-02，4MB Flash，CH340K） |
| 显示 | 板载 OLED（SSD1306 128×64，I2C GPIO5/6，地址 0x3C） |
| 485 | MAX485 自动收发型模块（DI=GPIO7 / RO=GPIO4），接原车 A/B 差分线 |
| 调试 | USB CDC（`Serial`，115200 8N1），与 485 完全隔离 |
| 轮速 | 霍尔 A3144 / SSR14Y（GPIO3，`INPUT_PULLUP` + 下降沿中断）+ 单个磁铁 |
| 轮胎 | 16x2.50/64-305，实测周长 **1.28 m** |

## 485 协议要点

```
68 [地址对] 68 [功能码] [长度] [数据 + 0x33] [校验和] 16
```

- 串口参数 **9600 8E1**（不是 115200 8N1）
- 校验和 = 从帧头累加到 CS 前一字节，取低 8 位（**sum & 0xFF，不是 CRC**）
- 请求帧的地址对 = 目的设备；**应答帧的地址对 = 回话设备自己**（真车：FOC 回 `20 DF`、BMS 回 `31 CE`、仪表 回 `10 EF`）
- 数据区每个字节发送时 `+0x33` 混淆，接收时 `-0x33` 反混淆

| 查询 | 应答 |
|------|------|
| `0x02` `05 08` | `0x82` + **12 字节**状态帧，车速协议值在**第 9 字节**（0-31），仪表按 ×12/25 换算 |
| `0x02` `05 09` | `0x82` + 14 字节状态帧 |
| `0x01` `02` | `0x81` + 版本串 `U1E02V11U1E02V11` |
| `0x01` `03` | `0x81` + MU 编号 `MU00001B00000001`（兼作解锁命令） |
| `0x01` `04` | `0x81` + `0x11`（固件版本） |
| `0x06` | `0x86` + 空数据 ACK（与原厂一致） |

## 仓库结构

```
niu/
├── README.md                       # 本文件
├── DEVELOPMENT.md                  # 开发记录：硬件选型、历史决策、已移除功能
├── docs/
│   ├── design_background.md        # 开发笔记（背景、需求、硬件选型）
│   ├── protocol_analysis.md        # 协议分析（帧格式、命令详解）
│   └── video_prompt.md             # AI 视频生成提示词（项目宣传用）
├── diagrams/                       # 接线拓扑图（SVG / PNG）
│   └── 2026-10-22T204500/
│       ├── diagram.svg
│       └── diagram.png
└── niu_u1_foc_simulator/           # PlatformIO 工程
    ├── platformio.ini
    ├── partitions.csv
    ├── README.md                   # 固件说明（接线、协议速查）
    ├── NOTES.md                    # 活文档：接线表 / 协议 / 已知问题，以它为准
    └── src/
        └── niu_u1_foc_simulator.ino
```

## 编译与烧录

```bash
cd niu_u1_foc_simulator
pio run          # 编译
```

依赖 U8g2。`platformio.ini` 默认指向本机 Arduino 库目录（离线可编），
换机器 / 在线编译时改成平台库名 `olikraus/U8g2@^2.36.19`。

> ⚠️ TW002 的 CH340K 没把 GPIO9（BOOT）接到 DTR，软件复位进不了下载模式，**必须手动按 BOOT**：
> 拔 USB → 按住 BOOT → 插回 USB → 立刻跑烧录命令 → 看到 "Writing at 0x..." 后松开。

## 参考资料

### 本项目文档

| 文档 | 地址 | 说明 |
|------|------|------|
| 接线与 GPIO 完整文档（飞书） | https://fcnm1ydb2e5g.feishu.cn/docx/XwprdaKRmoQgUixAL2ncKcq4nzc | 接线图、GPIO 定义、协议帧、异常排查（对外版） |
| 活文档 | [niu_u1_foc_simulator/NOTES.md](niu_u1_foc_simulator/NOTES.md) | 接线表 / 协议 / 已知问题，以它为准 |
| 固件说明 | [niu_u1_foc_simulator/README.md](niu_u1_foc_simulator/README.md) | 功能特性、硬件清单、协议速查 |
| 开发记录 | [DEVELOPMENT.md](DEVELOPMENT.md) | 开发方式与历史决策 |
| 协议分析 | [docs/protocol_analysis.md](docs/protocol_analysis.md) | 帧格式、命令详解 |
| 开发背景 | [docs/design_background.md](docs/design_background.md) | 需求分析、硬件选型 |

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
