/**
 * BLE 遥测通道配置
 *
 * 为什么要 BLE: OTA 走 WiFi 热点, 手机一连热点就没网了; 看车速不需要断网,
 * BLE 直连更顺手. 两条通道各干各的, 互不影响.
 */

#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#define BLE_ENABLE          1
// 名字出现在手机蓝牙列表里, 小程序按这个前缀找设备 (小程序那边是 'NIU-U1')
// 故意跟 WiFi 热点名 (NIU-U1-OTA) 区分开 —— 同名的话在手机蓝牙列表里分不清哪个是哪个
#define BLE_DEVICE_NAME     "NIU-U1-BLE"
// 沿用 Nordic UART 的 UUID 段, 免得跟别的 app 撞车
#define BLE_SERVICE_UUID    "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_TELEMETRY_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_FRAME_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
// 电池信息 (BMS 解析结果). 一条特征上跑多种消息, 用消息头区分, 见 ble_telemetry.h
#define BLE_BATTERY_UUID    "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
// 命令特征 (write): 小程序往里写单字节命令, 见 ble_telemetry.h 的 BleCommand
#define BLE_CMD_UUID        "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
// 485 总线日志特征 (notify): 手机点"保存日志"时, 设备把最近的总线帧分批发过来
#define BLE_LOG_UUID        "6e400006-b5a3-f393-e0a9-e50e24dcca9e"

// 遥测推送周期 (ms). 5Hz 足够看车速, 也够省电
#define BLE_NOTIFY_PERIOD_MS 200

// 定期重启广播的周期 (ms). 只在没连手机的时候做.
// 起因: 有一次重启广播正好撞上 GAP 状态机没准备好, 被静默丢掉 —— 板子还活着、
// 485 正常, 但 BLE 再也不广播, 手机永远扫不到. 这种状态没法自查, 只能定期重来.
#define BLE_REBROADCAST_PERIOD_MS 15000
// 单条 notify 的最大字节数. 蓝牙协商不出 MTU 时最小是 20, 帧长按这个切块
#define BLE_MAX_NOTIFY_LEN   20

#endif
