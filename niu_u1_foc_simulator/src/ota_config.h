// OTA 升级 (微信小程序 → ESP32 局域网直传) 相关配置
//
// 流程: 手机连 ESP32 的 SoftAP → 小程序 POST 固件到 /ota/* → 写进备用 OTA 分区 → 重启
// 详见 NOTES.md "OTA 升级" 章节

#define OTA_AP_SSID      "NIU-U1-OTA"
#define OTA_AP_PASSWORD  "niu12345"     // WPA2 最少 8 位
#define OTA_AP_IP        192,168,4,1    // 静态 IP, 小程序默认连这个
#define OTA_HTTP_PORT    8080

// 热点用的 WiFi 信道.
// 必须是 11: BLE 广播固定用 37/38/39 三个信道 (2402/2426/2428MHz),
// 而 WiFi 默认的信道 1 (2412MHz) 正好压在 BLE 广播信道 37 上 —— 共存时互相干扰,
// 表现就是手机能扫到热点但连不上, 或者连上就掉.
// 信道 11 = 2452MHz, 离 BLE 广播信道最远, 也避开了路由器常用的 1~6.
#define OTA_AP_CHANNEL   11

// 小程序每次 POST 的原始固件字节数上限 (base64 后约 x1.34)
#define OTA_CHUNK_SIZE   4096

// 固件版本号, 只通过 /ota/info 和 OLED 暴露.
// 不改 485 协议的 0x01 04 应答 (仍回 0x11), 免得破坏已经跑通的 V35 交互.
#define OTA_FW_VERSION   "1.4.1"

// 单个 app 槽的大小上限 —— 必须跟 partitions.csv 里的 app0/app1 一致.
// 超过就 Update.begin() 直接报 UPDATE_ERROR_SIZE.
#define OTA_MAX_FIRMWARE_SIZE 0x1D0000

// 0 = 实现里不编译 WiFi/OTA 代码
#define OTA_AP_ENABLE    1

// 1 = 开机不开热点, 蓝牙遥控开 (见 ble_telemetry.h 的 'W'/'X' 命令).
// 为什么默认关:
//   1) SoftAP 一直在发信标, 白耗电 (车上是电池供电)
//   2) 手机一连热点就断网 —— 看车速用 BLE 完全够, 手机还能照常上网
//   3) OTA 本来就一年碰不到几次, 需要时再开
//   4) 两边射频同时用的时候, BLE 广播间隔会被共存调度拉长, 手机扫码慢
// 实测 (hcitool lescan --duplicates, 12 秒): WiFi 关 100 个广播包, WiFi 开 117 个 ——
// 广播本身都正常, 但省电和不掉网的收益是真的.
#define OTA_AP_DEFAULT_OFF 1
