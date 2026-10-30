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
| 调试 | UART0（`Serial`，115200 8N1，经板载 CH340），与 485 完全隔离 |
| 轮速 | 霍尔 A3144 / SSR14Y（GPIO3，`INPUT_PULLUP` + 下降沿中断）+ 单个磁铁（脉冲间隔测速，轮周长 1.28 m）|
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
| `0x02` `05 08` | `0x82` + **12 字节**状态帧，车速协议值在**第 9 字节**（0-127，线性刻度 `round(kmh×25/12)`），仪表按 ×12/25 换算 |
| `0x02` `05 09` | `0x82` + 14 字节状态帧 |
| `0x01` `02` | `0x81` + 版本串 `U1E02V11U1E02V11` |
| `0x01` `03` | `0x81` + MU 编号 `MU00001B00000001`（兼作解锁命令） |
| `0x01` `04` | `0x81` + `0x11`（固件版本） |
| `0x06` | `0x86` + 空数据 ACK（与原厂一致） |

## OTA 升级（微信小程序）

车停楼下、人住 10 楼 —— 改个固件不该拆车接线再手动按 BOOT。

ESP32 开一个 WiFi 热点，手机连上它，**微信小程序通过局域网 HTTP 把新固件直接推进来**。
能这么干是因为微信小程序的「局域网例外」：`${IP}:${PORT}/${PATH}` 形式且手机与设备同网段时，
不做安全域校验、也不用 https —— 所以不需要备案域名 / 证书 / 云开发，ESP32 也不需要公网。

| 项 | 值 |
|---|---|
| 热点 | `NIU-U1-OTA` / `niu12345` |
| 地址 | `http://192.168.4.1:8080` |

> **热点默认是关的**（v1.4.0 起）。平时纯蓝牙，要升级时在小程序里点「用蓝牙开热点」，
> 固件会临时把 WiFi 拉起来；推完固件板子重启，就回到默认关的状态。
> 这样手机平时不会因为连热点而断网，车上也不用为一个用不上的热点耗电。

```
手机 (微信小程序)  ──蓝牙──►  ESP32-C3        ① 小程序写一个字节 'W'，固件开热点
  点「用蓝牙开热点」                              （'X' 是关热点）
手机 (微信小程序)  ──WiFi 热点──►  ESP32-C3     ② 手机连 NIU-U1-OTA
  选固件(内置/聊天)               SoftAP 192.168.4.1:8080
  base64 分块 POST  ─────────►   /ota/begin /ota/chunk /ota/end
  进度条 + KB/s                  Update.write → 备用槽 → 切槽重启
```

- 固件端：`niu_u1_foc_simulator/src/ota_server.h`（接口表见 [NOTES.md](niu_u1_foc_simulator/NOTES.md) 第 6 节）
- 小程序端：`miniprogram/`
- 端到端验证（不用手机）：`bash tools/ota_push_test.sh`
- 上传体验版 / 预览二维码：`bash tools/upload_miniprogram.sh preview|upload`（需要 AppID + 上传私钥）

> 分区表已改成 A/B 双槽（每槽 1.75MB），**必须全量重刷一次**（`tools/flash.sh`）之后 OTA 才可用。

## BLE 遥测（看车速不用连 WiFi）

OTA 走 WiFi 热点，手机一连热点就断网；看车速不需要断网，所以另开一条 BLE 通道：
GATT 服务把**车速 / 转速 / 电压 / 电流 / 协议车速 / 错误码**、**总线上抓到的 485 原始帧**，
以及**电池 BMS 的解析结果**推给小程序。

- 服务 UUID `6e400001-b5a3-f393-e0a9-e50e24dcca9e`，遥测特征 `…0002`，原始帧特征 `…0003`，电池特征 `…0004`，命令特征 `…0005`
- 遥测 5 Hz（200ms）主动通知；原始帧按 20 字节一节推送，小程序按帧尾 `0x16` 拼回整帧
- 命令特征 `…0005`（write）：`0x57` 开 OTA 热点、`0x58` 关热点 —— 蓝牙优先架构的遥控器
- 固件侧：`niu_u1_foc_simulator/src/ble_telemetry.h`
- 小程序侧：`miniprogram/pages/telemetry/`（大号车速 + 电池面板 + 485 帧滚动列表）

> **设备名必须放在主广播包里**（v1.4.0 起）。库默认把名字塞扫描回应，而 iPhone 后台的小程序
> 是**被动扫描**、收不到扫描回应，看到的就是「没名字的设备」被按名字过滤掉 ——
> 这就是之前 iPhone 扫不到的原因，Linux（主动扫描）一直看得到所以测不出来。

> 微信小程序**没有自动连 WiFi 的 API**（iOS / Android 都没有），所以没法替用户点「加入热点」。
> 小程序能做的是：连上热点后每 2 秒自动探测设备并连上，另外提供「复制热点密码」按钮一键填到系统设置里。
> 看车速这条需求用 BLE 就完全不需要 WiFi。

## 电池信息（485 总线上的 BMS 报文）

485 是半双工总线，V35 问 BMS「电量多少」时我们在旁边也听得见 —— 于是把 BMS 的应答解析出来给小程序看。
**我们不冒充 BMS、也不应答任何 BMS 请求**，只听。

V35 有四种问法，BMS 回三种东西：

| BMS 应答 | 内容 |
|---|---|
| 15 字节状态 | 总电压（大端 u16，0.1V）、电流（A）、电量（%）、5 个温度（°C） |
| 40 字节单体电压 | 每格 2 字节 mV（大端），13 串的 48V 电池就是 13 个 ~3.7V |
| 36 / 43 字节 | 电池型号串（16 字节 ASCII，如 `BUM9L62S43500598`） |

字段布局用真车抓包日志逐字段核对过：7 个日志共 2041 帧 BMS 报文，解析成功率 100%、校验和 0 错，
且状态帧的总电压与单体电压求和一致（48.6V vs 48.69V）。

- 固件侧：`niu_u1_foc_simulator/src/bms_protocol.h`（纯 C++，可在 PC 上编译跑测试）
- 详见 [NOTES.md](niu_u1_foc_simulator/NOTES.md) 第 6.6 节

> 顺带修了个分帧 bug：原来靠 `0x16` 找帧尾，但数据区里也可能出现 `0x16`（明文 `0xE3` + `0x33` 混淆后），
> 会把一帧劈成两半。现在改成按长度字节定帧。

## 仓库结构

```
niu/
├── README.md                       # 本文件
├── DEVELOPMENT.md                  # 开发记录：硬件选型、历史决策、已移除功能
├── package.json                    # 只有 miniprogram-ci 一个开发依赖
├── docs/
│   ├── design_background.md        # 开发笔记（背景、需求、硬件选型）
│   ├── protocol_analysis.md        # 协议分析（帧格式、命令详解）
│   └── video_prompt.md             # AI 视频生成提示词（项目宣传用）
├── diagrams/                       # 接线拓扑图（SVG / PNG）
│   └── 2026-10-22T204500/
│       ├── diagram.svg
│       └── diagram.png
├── miniprogram/                    # 微信小程序（OTA 客户端 + BLE 遥测）
│   ├── pages/index/                # 固件升级页
│   ├── pages/telemetry/            # 车辆状态页（BLE 车速 / 485 帧）
│   ├── utils/ota.js                # 分块上传
│   ├── utils/firmware.js           # 取固件（内置压缩模块 / 微信聊天选文件）
│   ├── utils/ble.js                # BLE 连接 + 订阅 + 解析（含电池消息分片重组）
│   ├── utils/lz.js                 # 固件解压（配套 tools/lz_pack.py）
│   └── firmware/firmware.js        # 构建产物（压缩+base64 固件），不入库
├── tools/
│   ├── build_ota.sh                # 编译固件 + 压缩 + 打包进小程序
│   ├── lz_pack.py                  # 固件压缩（极简 LZ77）
│   ├── flash.sh                    # 刷机（擦 otadata + bootloader + 分区表 + app）
│   ├── ota_push_test.sh            # curl 模拟小程序的端到端 OTA 测试
│   └── upload_miniprogram.sh       # miniprogram-ci 上传 / 预览二维码
└── niu_u1_foc_simulator/           # PlatformIO 工程
    ├── platformio.ini
    ├── partitions.csv               # A/B 双槽（每槽 1.75MB）
    ├── README.md                   # 固件说明（接线、协议速查）
    ├── NOTES.md                    # 活文档：接线表 / 协议 / 已知问题，以它为准
    └── src/
        ├── niu_u1_foc_simulator.ino
        ├── ota_config.h            # OTA 配置（SSID/密码/IP/端口/版本号/默认关热点）
        ├── ota_server.h            # OTA 服务端
        ├── ble_config.h            # BLE 配置（名字 / UUID / 通知周期 / 命令特征）
        ├── ble_telemetry.h         # BLE 遥测服务（车速 + 485 帧 + 电池 + 热点遥控命令）
        ├── ble_battery_msg.h       # 电池信息的 BLE 打包格式（纯 C++，可本机测）
        └── bms_protocol.h          # 电池 BMS 报文解析（纯 C++，可本机测）
```

> **固件为什么压缩后再 base64**：小程序主包上限 2MB，而固件加了 BLE 之后是 1.58MB，
> 直接 base64 是 2.1MB 超限。构建时用 `tools/lz_pack.py` 压到 ~1.05MB（68%），
> 小程序里用 `utils/lz.js` 解回原始固件再上传。

## 编译与烧录

```bash
cd niu_u1_foc_simulator
pio run          # 编译
```

依赖 U8g2。`platformio.ini` 默认指向本机 Arduino 库目录（离线可编），
换机器 / 在线编译时改成平台库名 `olikraus/U8g2@^2.36.19`。

> ⚠️ 2026-10-29 实测：TW002 刷机**不用手动按 BOOT 了** —— `tools/flash.sh` 用 esptool 的
> `--before default_reset`，通过 CH340 的 DTR/RTS 就能自动进下载模式（连刷三次都一次成功）。
> 手动按 BOOT 留作自动复位失灵时的备用：拔 USB → 按住 BOOT → 插回 USB → 立刻跑下面的命令。

```bash
bash tools/flash.sh /dev/ttyUSB0   # bootloader + 分区表 + app 一次写全
```

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
