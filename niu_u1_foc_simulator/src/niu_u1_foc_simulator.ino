/**
 * 小牛U1 FOC电机控制器模拟器 - ESP32-C3
 * 485 总线响应 V35 ECU 轮询 + 霍尔轮速 + OLED 速度显示
 */

#include <HardwareSerial.h>
#include <esp_log.h>
#include <esp_system.h>

// u8g2 每传一页都会调一次 Wire.setClock(), 而 u8g2 内部定的时钟是 100kHz ——
// 不定义这个宏的话, 我们自己设的 400kHz 每次都会被它改回 100kHz.
#define U8X8_DO_NOT_SET_WIRE_CLOCK
#include <U8g2lib.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "ota_config.h"
#include "ota_server.h"
#include "ble_config.h"
#include "hall_speed.h"
#include "ble_telemetry.h"
#include "bms_protocol.h"
#include "bus_log.h"

// ============ OLED 引脚定义 (板载I2C: SDA=GPIO5, SCL=GPIO6) ============
#define I2C_SDA_PIN 5
#define I2C_SCL_PIN 6
// U8G2_F 有完整的 getBufferPtr() 支持; U8G2_MIRROR = 水平镜像 (解决屏幕左右颠倒)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_MIRROR, /* reset=*/ U8X8_PIN_NONE);

// ============ 引脚定义 ============
// #define DE_RE_PIN 7    // 已停用：你那块 TTL→485 小板是"自动收发"型号，板内自含 RC 延时+三极管，
//                        //          不暴露 DE/RE 引脚，硬件自动切换发送/接收。
//                        //          若以后换了"半自动收发"或"裸 MAX485"小板，把这行注释打开即可。
#define WHEEL_SPEED_PIN 3
#define MAGNET_COUNT 1
// 轮胎 16x2.50/64-305, 实测转一圈 1.28 米 (不是标称值)
#define WHEEL_CIRCUMFERENCE_M 1.28

// ============ 轮速来源开关 ============
// 0 = 写死车速 (用固定值喂给 V35 ECU 调试通信链路)
// 1 = 霍尔实测 (装好磁钢后走这条路)
#define USE_HALL_SPEED    1
// 写死的车速, 单位 km/h × 10 (170 = 17.0 km/h)
// 仅在 USE_HALL_SPEED = 0 时生效; 17 km/h 对应协议值 35, 仪表显示约 16.8 km/h
#define HARDCODE_SPEED_DM 170

// 霍尔测速参数
#define HALL_DEBOUNCE_US      1000      // ISR 消抖: 1ms, 远小于最慢的真实脉冲间隔
// 物理上不可能的最小脉冲间隔 —— 毛刺闸门.
// 1 磁钢 + 1.28m 轮子: 30km/h 时真实间隔 154ms, 60km/h 时 77ms.
// 25ms 对应 ~92km/h, 也就是说真实脉冲永远不可能落在这个区间里 ——
// 落在里面的只能是传感器振铃 / 线受干扰产生的毛刺.
// 为什么需要它: 车上 485 和电机控制器是强干扰源, 传感器开漏输出用内部 45k 上拉
// 走长线, 边沿的振铃能通过 1ms 消抖. 一个毛刺会把一个真间隔劈成"几毫秒 + 剩下的"
// 两半, 毛刺占多数时 5 个样本的中位数就变成毛刺值 → 转速离谱 (实测 40 倍).
#define HALL_MIN_INTERVAL_US  25000
#define HALL_STOP_TIMEOUT_US  3000000   // 3s 没有新脉冲 → 认为停转, 车速归 0
// 毛刺闸门的**上限** —— 修"车速冻在 19.6km/h"的关键.
// 门槛一旦超过真脉冲可能的最小间隔, 就会开始丢**真**脉冲, 而且是自锁的:
// 真脉冲被当毛刺丢掉 (丢掉时不更新时间戳) → 这一拍量到的间隔翻倍 →
// 参照量跟着翻倍 → 门槛跟着涨 → 还是丢 → 越死越死.
// 车速就永久锁在真实值的一半 (19.6 = 39.2/2), 除非停下来.
// 上限的意义: 门槛永远 ≤ 80ms, 而 40km/h 的真间隔是 115ms → 真脉冲永远不会被误伤.
// 80ms 也高于实测的振铃偏移上限 (60ms), 所以毛刺照样被挡.
// 详见 src/hall_speed.h 的注释和 tools/test_hall_speed.cpp.
#define HALL_GATE_MAX_US      80000
// 停转阈值定 3s 是为了顾到低速骑行: 3s 转一圈 = 1.28m/3s = 1.5 km/h,
// 也就是低于 1.5km/h 仪表本来就看不出差别, 归零没有损失; 而 2s 的话
// 3~5km/h 的"蠕行"会整段显示 0 (间隔 1.5~2.3s 正好卡在阈值上).
#define HALL_HISTORY          5         // 取中位数的样本数 (奇数)

// ============ 联调测试开关 ============
// 485 每秒主动发一帧测试数据, 用来确认 ESP32 确实在往总线上发.
// 观察方式:
//   - USB-485 转换器接上 → 串口工具里能看到周期性字节
//   - 485 模块上的 TX 指示灯会跟着闪
// 联调完记得改回 0 —— 测试帧是协议里没有的功能码, 会干扰 V35 ECU 的正常轮询.
#define TEST_485_ENABLE      0
#define TEST_485_INTERVAL_MS 1000

// 调试开关: 从调试口 (UART0/CH340) 往 485 解析链路里塞一帧 —— 车不在手边时也能验 BMS 解析.
// 在串口里敲 "[TX] 68 31 CE 68 82 0F ... 16" 回车即可 (只认 [TX] 开头的行).
#define DEBUG_INJECT_485     1

// 485 收发的调试回显 (镜像到调试口). 关掉可以省掉串口上的开销,
// 车上跑的时候 USB 一般没插, 回显写进环形缓冲区就丢了, 不影响应答时序.
#define DEBUG_485_ECHO       1

// 板载 LED — 用来肉眼确认程序活着且在跑 loop.
// ESP32-C3 常见 LED 都在 GPIO8; 如果不亮, 改成 2 / 4 / 5 试 (别用 3/4/6/7, 那些被外设占了).
// 注意 GPIO8 是 strapping pin, 但 LED 串电阻接地不会影响正常启动 (只有 GPIO9 拉低才会进下载模式).
#define LED_PIN 8

// ============ 485 设备地址 (V35 ECU 私有协议 — 设备码 + 反码, 必须成对使用) ============
// 字节 1 和字节 2 永远成对出现, 像 IEC 62056 / DL/T 645 那样的定界符协议, 不是 Modbus 站号
// 协议里没有"发送方地址"字段 (485 半双工总线隐式知道谁在说话)
#define FOC_ADDR     0x20
#define FOC_ADDR_INV 0xDF
#define ECU_ADDR     0x40
#define ECU_ADDR_INV 0xBF
#define BMS_ADDR     0x31
#define BMS_ADDR_INV 0xCE
#define DASH_ADDR    0x10
#define DASH_ADDR_INV 0xEF

// 应答帧的地址对.
// 真车抓包 (xzfn/scooter-repair, 三台车上百帧): BMS 回 31 CE、仪表回 10 EF、FOC 回 20 DF ——
// 应答帧里的地址对标识的是"回话的设备自己", 所以 FOC 应答一律用本机地址对 20/DF.
// 若你的 V35 实测要求回 40/BF, 把下面这行改成 1.
#define REPLY_WITH_ECU_ADDR  0

// ============ 原厂 FOC 的固定身份串 ============
// V35 用 0x01 控制帧的子命令读 FOC 身份信息, 原厂 FOC 固定回 16 字节 ASCII.
// 抓包来源: github.com/xzfn/scooter-repair (U1 实车 485 抓包)
//   0x01 01 → 1 字节 0x00        状态标志
//   0x01 02 → 16 字节版本串      硬件 + 软件版本, 重复两遍
//   0x01 03 → 16 字节 MU 编号    电机编号, 同时兼作解锁命令
//   0x01 04 → 1 字节 0x11        固件版本
#define FOC_MU_STRING      "MU00001B00000001"
#define FOC_VERSION_STRING "U1E02V11U1E02V11"

// 功能码
#define FUNC_QUERY         0x02
#define FUNC_RESPONSE      0x82
#define FUNC_CONTROL       0x01
#define FUNC_CONTROL_ACK   0x81
#define FUNC_SET_PARAM     0x05
#define FUNC_SET_PARAM_ACK 0x85
#define FUNC_WRITE_PARAM     0x04
#define FUNC_WRITE_PARAM_ACK 0x84
#define FUNC_CONFIG_SYSTEM   0x06
#define FUNC_CONFIG_SYSTEM_ACK 0x86

// ============ FOC状态 ============
struct FOCStatus {
    uint16_t voltage;
    int16_t current;
    uint16_t foc_speed;
    uint16_t wheel_rpm;
    uint16_t wheel_speed_dm;   // 车速 × 10 (十分位整数, 保留小数显示精度)
    uint8_t error;
    uint8_t unlocked;
    unsigned long unlock_timestamp;
} __attribute__((packed));

static FOCStatus foc_status = {
    .voltage = 480, .current = 0, .foc_speed = 0,
    .wheel_rpm = 0, .wheel_speed_dm = 0,
    .error = 0x00, .unlocked = 0, .unlock_timestamp = 0
};

#if BLE_ENABLE
// BLE 遥测实例 —— 声明要放在 FOCSimulator 前面, parseFrame 里要用
BleTelemetry bleTelemetry;
#endif

// ============ 电池 BMS ============
// 总线上 V35 挨个轮询, BMS(0x31) 的应答我们只在旁边听着 —— 不参与应答,
// 但把电压/电流/电量/温度/单体电压解析出来给小程序看.
static BmsInfo bms_info = {0};
static unsigned long bms_last_log = 0;

// BMS 诊断计数 —— 回答"小程序上为什么没有电池数据".
// 有了这几个数就能一眼分清三种情况:
//   rxFrames == 0              → 总线上根本没有 BMS 应答 (BMS 没回话, 或我们的分帧把它吃了)
//   rxFrames > 0, okFrames == 0 → 帧收到了但解析不出来 (校验和错 = 被干扰/被撞坏)
//   okFrames > 0               → 固件这边完全正常, 问题在蓝牙这一段
static BmsDiag bms_diag = {0};
static unsigned long bms_last_rx_ms = 0;
static bool bms_seen = false;

// ============ 轮速测量 ============
// 单磁钢 + 计数窗口测不出速度: 17km/h 时 250ms 窗口只期望 0.9 个脉冲, 读数会在
// 0 和 ~24km/h 之间乱跳. 所以改用"脉冲间隔"测速 —— 相邻两个脉冲之间正好转了
// MAGNET_COUNT 圈, 17km/h 时间隔 270ms, 分辨率 0.4%.
//
// 但"直接用最近一次间隔"有两个坑, 表现就是"有时候没有速度":
//   1) 漏脉冲: 磁钢经过传感器那一下没被捕捉到 (颠簸/干扰), 间隔直接变 2 倍
//      → 车速凭空减半, 下一拍又跳回去
//   2) 抖动: 传感器输出毛刺, 一个真间隔被劈成两半
//      → 车速凭空翻倍
// 修法: 保留最近 HALL_HISTORY 个间隔取**中位数**. 中位数天生抗离群值 ——
// 漏一个脉冲 (5 个里 1 个是 2 倍) 不影响中位数, 毛刺劈出的两个半间隔也一样.
// 代价是车速变化有 2~3 拍的滞后 (17km/h 时约 0.8s), 换来的是稳.
// 测速的逻辑全在 src/hall_speed.h 里 (不依赖 Arduino, 可以在 PC 上做单元测试).
// ⚠️ 不要在这里另写一份 —— 车速"冻在 19.6km/h" 就是这么查出来的:
//    tools/test_hall_speed.cpp 复现了那个锁死, 改完记得跑它.
static HallSpeed hallSpeed;

// 给别处用的别名 (诊断 / 日志 / BLE 遥测)
volatile uint32_t& wheel_pulse_count = hallSpeed.pulse_count;
volatile uint32_t& hall_glitch_count = hallSpeed.glitch_count;

// 协议速度值 — 线性刻度, 1 格 = 12/25 = 0.48 km/h
// 依据:
//   1) 实车跑通的 ECU 实现 github.com/xzfn/scooter-repair niu-ecu/User/niu_series_u.c
//        dash_request.speed = foc_speed * 12 / 25;
//        g_mileage += foc_speed * 12 * dt_ms * 10 / 36 / 25;
//   2) 抓包统计: FOC 车速字节最大到 44, 仪表字节正好是它 *12/25 (±1)
// 所以协议值 = round(kmh * 25 / 12), 取值 0..127 (int8, 负数 = 倒挂)
uint16_t calcFOCSpeedFromKMH(float kmh) {
    uint32_t v = (uint32_t)(kmh * 25.0f / 12.0f + 0.5f);   // 四舍五入
    if (v > 127) v = 127;
    return (uint16_t)v;
}

// 轮速更新由 loop 周期驱动 (不是只在收到查询时), OLED 才能显示实时速度
static unsigned long lastSpeedCalc = 0;
// 给别处用的别名 (诊断 / 日志 / BLE 遥测) —— 实际值都在 hallSpeed 里
uint32_t& last_interval_us = hallSpeed.last_interval_us; // 最近一次测到的脉冲间隔, 只用于日志
uint32_t& hall_median_us = hallSpeed.median_us;         // 算转速实际用的那个间隔 (中位数), 只用于日志
uint32_t& hall_gap_reset = hallSpeed.gap_reset;         // 因停车太久重置过历史的次数 (诊断用)

// 测速的间隔/毛刺/中位数逻辑全在 src/hall_speed.h 里 (不依赖 Arduino, 可在 PC 上单测).
// 这里只留一个包装, 免得 .ino 里再抄一份 —— 抄一份的话, 单测测的就不是车上的代码了.
// 详见 src/hall_speed.h 的注释 和 tools/test_hall_speed.cpp.
void updateWheelSpeed() {
    unsigned long now = millis();
    unsigned long dt = now - lastSpeedCalc;
    if (dt < 100) return;
    lastSpeedCalc = now;

    float kmh = 0.0f;
    float rpm = 0.0f;

#if USE_HALL_SPEED
    // 收新间隔 + 算车速. 逻辑在 hall_speed.h 里 (单测覆盖).
    uint32_t now_us = micros();
    hallSpeed.poll(now_us);
    hallSpeed.speed(now_us, &kmh, &rpm);
#else
    // 霍尔未启用: 直接给车速, 后面整条链路 (显示 + 协议值) 逻辑不变
    kmh = HARDCODE_SPEED_DM / 10.0f;
    rpm = kmh * 1000.0f / (WHEEL_CIRCUMFERENCE_M * 60.0f);
#endif

    foc_status.wheel_rpm = (uint16_t)rpm;
    foc_status.wheel_speed_dm = (uint16_t)(kmh * 10.0f);
    foc_status.foc_speed = calcFOCSpeedFromKMH(kmh);
}

void IRAM_ATTR wheelPulseISR() {
    // 逻辑在 hall_speed.h 里 (单测覆盖): 1ms 消抖 + 自适应毛刺闸门.
    // ⚠️ 丢毛刺时**不更新** last/prev 时间戳 —— 一个毛刺把一个真间隔劈成两半的话,
    //    车速会一头高一头低. 详见 src/hall_speed.h 的第 1 条.
    hallSpeed.onPulse(micros());
}

static uint8_t foc_response_data[14] = {0};

// 原厂 FOC 状态帧布局 (12 字节).
// 来源: github.com/xzfn/scooter-repair 的 niu-ecu/User/niu_series_u.h
//       struct PayloadFOCInfoResponse (能实车跑通的 ECU 实现, 长度必须正好 12)
//   [0..2] 常量 30 26 3C
//   [3]    flag_0: 0x01=驱动模式1, 0x02=驱动模式2
//   [4]    保留
//   [5]    保留 (实车恒为 0x19)
//   [6..8] 保留
//   [9]    车速协议值 (0-127, int8; 负数=倒挂). ECU 换算: 仪表速度 = 值*12/25
//   [10]   flag_1: 0x08=驻车, 0x10=能量回收
//   [11]   保留
// 查询子命令 05 09 的原厂 FOC 多回 2 字节 (00 02), 共 14 字节.
#define FOC_INFO_FLAG_0_DRIVE_MODE_1 0x01
#define FOC_INFO_FLAG_1_PARKING      0x08

class FOCSimulator {
private:
    HardwareSerial* serial;

    void setTransmitMode(bool enable) {
        // 自动收发模块：硬件自动切换方向，软件什么都不做
        // 仅保留函数签名以便未来切换到手动 DE/RE 模块时复用
        (void)enable;
    }

    uint8_t calcChecksum(const uint8_t* data, size_t len) {
        uint16_t sum = 0;
        for (size_t i = 0; i < len; i++) sum += data[i];
        return sum & 0xFF;
    }

    void sendFrame(uint8_t func, const uint8_t* data, uint8_t data_len) {
        uint8_t frame[64];
        size_t idx = 0;
        frame[idx++] = 0x68;
#if REPLY_WITH_ECU_ADDR
        frame[idx++] = ECU_ADDR;      // 回 ECU 的地址对
        frame[idx++] = ECU_ADDR_INV;
#else
        frame[idx++] = FOC_ADDR;      // 应答帧带本机 (FOC) 地址对, 与真车一致
        frame[idx++] = FOC_ADDR_INV;
#endif
        frame[idx++] = 0x68;
        frame[idx++] = func;
        frame[idx++] = data_len;
        for (uint8_t i = 0; i < data_len; i++) {
            frame[idx++] = data[i] + 0x33;   // 数据区 +0x33 混淆
        }
        frame[idx++] = calcChecksum(frame, idx);
        frame[idx++] = 0x16;
        // 记进 485 总线日志 (手机点"保存日志"时会把这些拉走做离线分析).
        // 带上 TX 是关键: 只有同时看到"V35 的问"和"我们回的答"跟时间戳,
        // 才能算出应答延迟 —— 而应答延迟正是 V35 超时 (错误码 190) 的根因.
        busLog().record(BUS_LOG_DIR_TX, millis(), frame, idx);
        setTransmitMode(true);
        serial->write(frame, idx);
        serial->flush();
#if DEBUG_485_ECHO
        // 调试回显：镜像到调试口 (独立通道，不污染 485).
        // 刻意不调 Serial.flush() —— 电脑接上但没人读时, 串口 TX 会堵住,
        // 而 V35 的单次轮询超时只有 ~20ms, 卡一次这一轮就黄了.
        Serial.printf("[485→] ");
        for (size_t i = 0; i < idx; i++) Serial.printf("%02X ", frame[i]);
        Serial.printf("\n");
#endif
        // 自动收发模块切换有 ~1-2ms 延迟；flush 后等一下让总线稳定
        delay(3);
        setTransmitMode(false);
    }

    void updateFOCResponse(bool extended) {
        foc_response_data[0] = 0x30;
        foc_response_data[1] = 0x26;
        foc_response_data[2] = 0x3C;
        foc_response_data[3] = FOC_INFO_FLAG_0_DRIVE_MODE_1;
        foc_response_data[4] = 0x00;
        foc_response_data[5] = 0x19;
        foc_response_data[6] = 0x00;
        foc_response_data[7] = 0x00;
        foc_response_data[8] = 0x00;
        foc_response_data[9] = (uint8_t)foc_status.foc_speed;   // 车速协议值 (0-127)
        foc_response_data[10] = (foc_status.foc_speed == 0) ? FOC_INFO_FLAG_1_PARKING : 0x00;
        foc_response_data[11] = 0x00;
        if (extended) {
            foc_response_data[12] = 0x00;
            foc_response_data[13] = 0x02;
        }
    }

public:
    FOCSimulator(HardwareSerial* s) : serial(s) {}

    // 诊断计数 —— 小程序上要看"到底是谁的问题":
    //   q_count  收到 V35 的状态查询次数 (05 08 / 05 09)
    //   a_count  我们回出去的应答次数 (应该跟 q_count 一致)
    //   bad_cs   校验和对不上的帧 (总线噪声/干扰)
    //   oth      其它功能码的查询次数 (解锁/配置等)
    uint16_t q_count = 0;
    uint16_t a_count = 0;
    uint16_t bad_cs = 0;
    uint16_t oth = 0;

    // 联调用: 主动往 485 总线发一帧, 确认 ESP32 确实在发.
    // 用的是协议里没定义过的功能码 0xEE, 不会跟 V35 ECU 的正常轮询混淆.
    void sendTestFrame() {
        uint8_t test_data[] = { 0xAA, 0x55, 0xDE, 0xAD };
        sendFrame(0xEE, test_data, sizeof(test_data));
    }

    void begin() {
        setTransmitMode(false);
        // 霍尔传感器 (A3144/SSR14Y) 是开集电极输出, 必须上拉才有确定高电平
        pinMode(WHEEL_SPEED_PIN, INPUT_PULLUP);
        attachInterrupt(WHEEL_SPEED_PIN, wheelPulseISR, FALLING);
    }

    void handleQuery(const uint8_t* data, uint8_t len) {
        if (len < 2) return;
        // 05 08 = U1 Sport / Civic / M1 Sport 的状态查询 → 原厂回 12 字节
        // 05 09 = U M-Sport 的状态查询 → 原厂回 14 字节
        if (data[0] == 0x05 && data[1] == 0x08) {
            updateFOCResponse(false);
            q_count++;
            sendFrame(FUNC_RESPONSE, foc_response_data, 12);
            a_count++;
        } else if (data[0] == 0x05 && data[1] == 0x09) {
            updateFOCResponse(true);
            q_count++;
            sendFrame(FUNC_RESPONSE, foc_response_data, 14);
            a_count++;
        }
    }

    void handleControl(const uint8_t* data, uint8_t len) {
        if (len < 1) return;
        switch (data[0]) {
            case 0x01: {  // 读状态标志 → 1 字节 0x00
                uint8_t ack[] = {0x00};
                sendFrame(FUNC_CONTROL_ACK, ack, sizeof(ack));
                break;
            }
            case 0x02:   // 读硬件/软件版本 → 16 字节 ASCII
                sendFrame(FUNC_CONTROL_ACK, (const uint8_t*)FOC_VERSION_STRING, 16);
                break;
            case 0x03:   // 读 MU 编号 → 16 字节 ASCII (原厂兼作解锁命令)
                foc_status.unlocked = 1;
                foc_status.unlock_timestamp = millis();
                sendFrame(FUNC_CONTROL_ACK, (const uint8_t*)FOC_MU_STRING, 16);
                break;
            case 0x04: { // 读固件版本 → 1 字节
                uint8_t ack[] = {0x11};
                sendFrame(FUNC_CONTROL_ACK, ack, sizeof(ack));
                break;
            }
            case 0x38:  // V35 读 FOC 额定电压
            case 0x3C:  // V35 读 FOC 额定电流
                // 静默处理：V35 后续会发 0x02 查询具体值
                break;
            default:
#if DEBUG_485_ECHO
                Serial.printf("[V35] unknown control: 0x%02X len=%d\n", data[0], len);
#endif
                break;
        }
    }

    void handleSetParam(const uint8_t* data, uint8_t len) {
        uint8_t ack_data[] = { 0xCE };
        sendFrame(FUNC_SET_PARAM_ACK, ack_data, 1);
    }

    void handleWriteParam(const uint8_t* data, uint8_t len) {
        // V35 数据写入 (0x04) — 只 ACK
#if DEBUG_485_ECHO
        Serial.printf("[V35] FUNC_WRITE_PARAM: %d bytes\n", len);
#endif
        uint8_t ack_data[] = { 0xCD };
        sendFrame(FUNC_WRITE_PARAM_ACK, ack_data, 1);
    }

    void handleConfigSystem(const uint8_t* data, uint8_t len) {
        // V35 系统配置 (0x06) — 主/从模式等
        uint8_t subcmd = len > 0 ? data[0] : 0xFF;
#if DEBUG_485_ECHO
        const char* mode = "?";
        switch (subcmd) {
            case 0x01: mode = "set_slave"; break;
            case 0x02: mode = "set_master"; break;
            default:    mode = "other";    break;
        }
        Serial.printf("[V35] FUNC_CONFIG_SYSTEM: subcmd=0x%02X (%s)\n", subcmd, mode);
#endif
        // 原厂 FOC 对 0x06 的确认是空数据 (抓包: 68 20 DF 68 86 00 CS 16)
        static const uint8_t empty[] = {0x00};
        sendFrame(FUNC_CONFIG_SYSTEM_ACK, empty, 0);
    }

    void parseFrame(const uint8_t* raw_frame, size_t len) {
        if (len < 9 || raw_frame[0] != 0x68 || raw_frame[len-1] != 0x16) return;
#if BLE_ENABLE
        // 总线上抓到的帧原样推给手机 —— 不管是发给谁的问询帧都推
        bleTelemetry.pushFrame(raw_frame, len);
#endif
        // 电池 BMS 的应答 —— 我们不是 BMS, 不应答, 只在旁边听并解析.
        // 注意: 地址对是 31 CE 的帧有两种 —— V35 问 BMS 的**请求** (功能码 0x02),
        // 和 BMS 回 V35 的**应答** (功能码 0x82, 最高位是 1). 只有后者才是"BMS 回话了",
        // 统计的时候要分清楚, 不然会把"看到 40 帧"误当成"收到 40 次应答".
        if (raw_frame[1] == BMS_ADDR) {
            int st = bmsParseFrame(raw_frame, len, &bms_info);
            if (raw_frame[4] & 0x80) {          // 是应答, 不是请求
                bms_diag.rxFrames++;
                bms_diag.lastLen = (uint8_t)len;
                bms_last_rx_ms = millis();
                bms_seen = true;
                if (st == BMS_PARSE_OK)              bms_diag.okFrames++;
                else if (st == BMS_PARSE_BAD_CS)     bms_diag.csErr++;
                else if (st == BMS_PARSE_BAD_LEN)    bms_diag.lenErr++;
                else if (st == BMS_PARSE_UNKNOWN)    bms_diag.unknownLen++;
            }
            return;
        }
        uint8_t addr = raw_frame[1];
        uint8_t addr_inv = raw_frame[2];
        uint8_t func = raw_frame[4];
        uint8_t data_len = raw_frame[5];
        // 校验: 字节 2 必须是字节 1 的协议定义反向字节 (FOC=DF, ECU=BF, BMS=CE, Dash=EF)
        if ((addr ^ 0xFF) != addr_inv) return;
        // 字节 3 必须是 0x68 (定界符 / 同步), 不校验会被噪声帧误判
        if (raw_frame[3] != 0x68) return;
        // 只处理发往本设备 (FOC) 的帧
        if (addr != FOC_ADDR) return;
        // 校验和 (协议是"从头累加到 CS 前一字节, 取低 8 位", 不是 CRC).
        // 不校验的话, 总线上的噪声/干扰会被当成一个查询, 我们回一帧,
        // 反而把 V35 的应答时序搞乱 —— 宁可漏答, 不能乱答.
        uint8_t cs = 0;
        for (size_t i = 0; i + 2 < len; i++) cs = (uint8_t)(cs + raw_frame[i]);
        if (cs != raw_frame[len - 2]) {
            bad_cs++;
            return;
        }
        uint8_t data[32] = {0};
        uint8_t actual_len = min(data_len, (uint8_t)32);
        for (uint8_t i = 0; i < actual_len; i++) {
            data[i] = raw_frame[6 + i] - 0x33;   // 数据区 -0x33 反混淆
        }
        switch (func & 0x7F) {
            case FUNC_QUERY:           handleQuery(data, actual_len); break;
            case FUNC_CONTROL:         handleControl(data, actual_len); oth++; break;
            case FUNC_SET_PARAM:       handleSetParam(data, actual_len); oth++; break;
            case FUNC_WRITE_PARAM:     handleWriteParam(data, actual_len); oth++; break;
            case FUNC_CONFIG_SYSTEM:   handleConfigSystem(data, actual_len); oth++; break;
        }
    }
};

FOCSimulator simulator(&Serial1);

#if OTA_AP_ENABLE
OtaService otaService;

#if BLE_ENABLE
static void bleApToggle(bool on) {
    if (on) otaService.startAP();
    else    otaService.stopAP();
}
#endif
#endif

#if BLE_ENABLE
// 小程序在「车辆」页要显示当前固件版本. 平时 WiFi 是关的, 问不到 /ota/info,
// 所以走蓝牙: 小程序写一个 'V', 这里把版本号回过去.
static void bleVersionProvider(char* out, size_t cap) {
    if (!out || cap == 0) return;
    strncpy(out, OTA_FW_VERSION, cap - 1);
    out[cap - 1] = '\0';
}
#endif

static uint8_t rx_buf[256];
static size_t rx_len = 0;
static unsigned long rx_first_byte_ms = 0;   // 当前帧第一字节到达的时刻 (日志用)
static unsigned long last_debug = 0;
#if DEBUG_INJECT_485
static char injectBuf[512];
static size_t injectLen = 0;
static int injectHexPos = 0;
static int injectHi = 0;
#endif

// ============ 485 接收: 按长度字节定帧 ============
// 不要靠 0x16 找帧尾: 数据区里也可能出现 0x16 (明文 0xE3 + 0x33 混淆后正好是 0x16),
// 靠 0x16 分帧会把一帧劈成两半, 后面那一半会被当成新帧, 结果是 BMS 的应答解析不出来.
// 正确做法: 第 6 个字节是数据长度, 整帧长度 = 6 + len + 2 (校验和 + 0x16).
static void rxResync() {
    size_t i = 1;
    while (i < rx_len && rx_buf[i] != 0x68) i++;
    if (i < rx_len) {
        memmove(rx_buf, rx_buf + i, rx_len - i);
        rx_len -= i;
    } else {
        rx_len = 0;
    }
#if DEBUG_485_ECHO
    Serial.printf("\n[485] 重新同步\n");
#endif
}

static void rxPush(uint8_t c) {
    if (rx_len == 0) {
        // 总线空闲时会有 fe / 55 之类的填充字节, 不是 0x68 就丢掉
        if (c != 0x68) return;
        rx_first_byte_ms = millis();   // 帧的第一字节到达的时刻 (日志用)
#if DEBUG_485_ECHO
        Serial.printf("[485←] ");
#endif
        rx_buf[rx_len++] = c;
#if DEBUG_485_ECHO
        Serial.printf("%02X ", c);
#endif
        return;
    }
    if (rx_len >= sizeof(rx_buf)) { rxResync(); return; }
    rx_buf[rx_len++] = c;
#if DEBUG_485_ECHO
    Serial.printf("%02X ", c);
#endif

    if (rx_len == 4) {
        if (rx_buf[3] != 0x68) { rxResync(); return; }   // 不是帧
        return;
    }
    if (rx_len < 6) return;

    size_t total = 6 + (size_t)rx_buf[5] + 2;
    if (total > sizeof(rx_buf)) { rxResync(); return; }   // 长度离谱, 重新同步
    if (rx_len == total) {
        // 先应答, 后回显 —— V35 的单次轮询超时只有 ~20ms,
        // 任何阻塞式的 USB flush 都可能把这一轮拖成超时 (表现为车速 0 + 错误码 190)
#if DEBUG_485_ECHO
        Serial.printf("\n");
#endif
        // 记进 485 总线日志. 时间戳用**第一字节到达**的时刻, 不是凑齐的时刻 ——
        // 差的这几毫秒正好是"应答延迟"里最要紧的一段.
        busLog().record(BUS_LOG_DIR_RX, rx_first_byte_ms, rx_buf, rx_len);
        simulator.parseFrame(rx_buf, rx_len);
        rx_len = 0;
    }
}

// ============ OLED 显示 ============
// 显示放到独立任务里跑, 原因很硬:
// 一屏 1024 字节, 100kHz 下要传 ~95ms. 要是放在 loop 里画, 这 95ms 里 loop 完全动不了 ——
// 而 V35 单次轮询的超时只有 ~20ms, 撞上了就是"这一轮没应答 → 错误码 190 → 车速 0",
// 也就是"有时候没有速度". 挪到独立任务后, I2C 传输期间 loop 照样跑, 应答只晚 ~1ms.
#define OLED_TASK_STACK 4096
#define OLED_TASK_PRIO  1      // 跟 Arduino 的 loop 任务同级, 靠时间片轮转
#define OLED_REFRESH_MS 200
#define OLED_I2C_HZ     400000  // SSD1306 支持 400kHz, 传输时间从 95ms 降到 ~25ms
// 1 = 每 200ms 无条件重画屏幕 (测"显示满负荷时 485 应答还准不准"用)
#define OLED_FORCE_REDRAW 0

// 显示用的数据快照 —— loop 任务写, OLED 任务读, 用互斥量保护
struct DispState {
    uint16_t speed_dm;   // 车速 ×10
    uint16_t rpm;
    uint8_t  error;
    uint8_t  soc;        // 0xFF = 还没拿到电池数据
    uint8_t  unlocked;
};
static DispState disp_state = {0, 0, 0, 0xFF, 0};
static SemaphoreHandle_t disp_lock = nullptr;

// 开机画面要显示复位原因 —— 车上如果是在棕断/看门狗复位, 这里一眼能看出来
static const char* g_rst_reason = "?";
static uint32_t g_boot_ms = 0;

// 只显示速度: 大字 km/h + 底部 RPM / 错误码
// (不把 DispState 写进参数里: .ino 的原型生成器会把函数原型插到文件最前面,
//  那时候这个结构体还没定义, 编译不过. 所以这里直接读全局快照.)
void drawOled() {
    DispState s;
    if (!disp_lock || xSemaphoreTake(disp_lock, portMAX_DELAY) != pdTRUE) return;
    s = disp_state;
    xSemaphoreGive(disp_lock);

    u8g2.clearBuffer();
    u8g2.setDrawColor(1);

    // 大字车速 (十分位整数 → "23.4")
    char spd[16];
    snprintf(spd, sizeof(spd), "%u.%u", s.speed_dm / 10, s.speed_dm % 10);
    u8g2.setFont(u8g2_font_ncenB14_te);
    int w = u8g2.getStrWidth(spd);
    u8g2.setCursor((128 - w) / 2, 34);
    u8g2.print(spd);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor((128 - w) / 2 + w + 4, 24);
    u8g2.print("km/h");

    // 复位原因 —— 常驻显示. 车上要是"屏幕一会亮一会不亮",
    // 那多半是板子在反复复位 (棕断/看门狗/panic), 这一行直接告诉你上次是怎么挂的.
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 43);
    u8g2.printf("rst:%s", g_rst_reason);

    // 底部: 电量 + RPM + 错误码
    char bottom[32];
    if (s.soc != 0xFF) {
        snprintf(bottom, sizeof(bottom), "%d%%  %d RPM", s.soc, (int)s.rpm);
    } else {
        snprintf(bottom, sizeof(bottom), "%d RPM", (int)s.rpm);
    }
    u8g2.setCursor(0, 55);
    u8g2.print(bottom);
    if (s.error != 0) {
        char err[8];
        snprintf(err, sizeof(err), "ERR%02X", s.error);
        w = u8g2.getStrWidth(err);
        u8g2.setCursor(128 - w, 55);
        u8g2.print(err);
    }

    u8g2.sendBuffer();
}

// 开机画面: 版本 + 复位原因 + 热点地址
void drawOledBoot() {
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 4);
    u8g2.printf("NIU-U1 FOC  v%s", OTA_FW_VERSION);
    u8g2.setCursor(0, 18);
    u8g2.printf("rst: %s", g_rst_reason);
    u8g2.setCursor(0, 30);
    u8g2.printf("ap: %s", OTA_AP_SSID);
    u8g2.setCursor(0, 42);
    u8g2.printf("ip: %s", WiFi.softAPIP().toString().c_str());
    u8g2.sendBuffer();
}

// OTA 传输中抢下屏幕: 显示热点名/IP/进度, 让人一眼知道在干嘛
void drawOledOta() {
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_6x10_tf);

    u8g2.setCursor(0, 2);
    u8g2.printf("OTA  %s", OTA_AP_SSID);

    u8g2.setCursor(0, 14);
    u8g2.printf("IP %s:%d", WiFi.softAPIP().toString().c_str(), OTA_HTTP_PORT);

    // 进度条
    uint32_t total = otaService.total();
    uint32_t cur = otaService.progress();
    u8g2.drawFrame(0, 24, 128, 12);
    int w = total ? (int)((124 * (uint32_t)cur) / total) : 0;
    if (w > 0) u8g2.drawBox(2, 26, w, 8);

    u8g2.setCursor(0, 40);
    u8g2.printf("%u/%u KB  %d%%", cur / 1024, total / 1024,
                total ? (int)((uint32_t)cur * 100 / total) : 0);

    u8g2.setCursor(0, 52);
    u8g2.print("keep this page open");

    u8g2.sendBuffer();
}

// OLED 任务 —— 只碰 u8g2 和自己那份快照
void oledTask(void* arg) {
    DispState last = {0xFFFF, 0xFFFF, 0xFF, 0xFE, 0xFF};   // 强制第一次一定画
    unsigned long lastLog = 0;
    for (;;) {
        DispState s = last;
        if (xSemaphoreTake(disp_lock, portMAX_DELAY) == pdTRUE) {
            s = disp_state;
            xSemaphoreGive(disp_lock);
        }

        unsigned long t0 = micros();
        bool drew = false;
        if (millis() - g_boot_ms < 2500) {
            drawOledBoot();
            drew = true;
        } else if (otaService.isBusy()) {
            drawOledOta();
            drew = true;
        } else if (memcmp(&s, &last, sizeof(s)) != 0 || OLED_FORCE_REDRAW) {
            drawOled();   // 内容变了才重画 (省掉一次 1KB 的 I2C 传输)
            drew = true;
        }
        last = s;
        if (drew && millis() - lastLog > 5000) {
            lastLog = millis();
            Serial.printf("[OLED] 刷屏耗时 %lu ms (I2C %d kHz)\n",
                          (unsigned long)((micros() - t0) / 1000), OLED_I2C_HZ / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_MS));
    }
}

void setup() {
    disableLoopWDT();

    // 调试日志走 UART0 (板载 CH340) —— 注意 platformio.ini 里的 CDC_ON_BOOT=1 对 C3 无效,
    // core 只认 ARDUINO_USB_CDC_ON_BOOT, 所以 Serial 就是 UART0
    // 独立于 485 总线, 调试 ASCII 不会污染 V35 总线
    Serial.begin(115200);
    delay(100);  // 等 USB 枚举

    Serial.print("=== 小牛U1 FOC模拟器 ===\n"); Serial.flush();

    // Wire 必须最早初始化 — 让 FreeRTOS mutex 和 ESP-IDF I2C0 driver 在最早可用时就绪,
    // 否则 u8g2 首次传输时 I2C0 driver 还没就绪, 会刷 "could not acquire lock" +
    // "NULL TX buffer pointer" 错误.
    // 时钟用 400kHz: 一屏 1024 字节, 100kHz 要 95ms, 400kHz 只要 ~25ms.
    // 注意 u8g2 自己会调 Wire.setClock(100000) 把时钟改回去 —— 靠文件顶部的
    // U8X8_DO_NOT_SET_WIRE_CLOCK 关掉它.
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(OLED_I2C_HZ);

    // 485 总线 (V35 ECU 通信) - 9600 8E1, GPIO4=RX / GPIO7=TX, 自动收发模块
    // GPIO2/8/9 是 strapping pin, GPIO11-17/18/19/20/21 被 flash/USB/UART0 占用,
    // 所以只用安全引脚 3/4/5/6/7/10.
    Serial1.begin(9600, SERIAL_8E1, 4, 7);
    delay(10);
    Serial.print("[485] Serial1 已启动 9600 8E1 (RX=GPIO4, TX=GPIO7)\n"); Serial.flush();

    // 静音 ESP-IDF 内部日志。
    // 关键: Arduino-ESP32 的 log_v()/log_d() 不走 ESP_LOGv 而直接调 log_printf,
    //       所以 esp_log_level_set() 对它们无效 — 真正的兜底是 platformio.ini 里的
    //       -DCORE_DEBUG_LEVEL=3 (编译期把 log_v/log_d 替换成空操作)。
    //       下面这组只管 ESP-IDF 原生日志 (wifi/phy), 留作 defense in depth.
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_NONE);

    simulator.begin();

    // 板载 LED — 联调时用肉眼确认程序在跑
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

#if USE_HALL_SPEED
    Serial.printf("[FOC] 霍尔中断已挂载 (GPIO3, INPUT_PULLUP) — 实测模式: %d 磁钢, 周长 %.2fm\n",
        MAGNET_COUNT, WHEEL_CIRCUMFERENCE_M);
    Serial.printf("[FOC] 测速方式: 脉冲间隔 (相邻两脉冲 = 转 %d 圈), 无脉冲超过 2s 判停转\n",
        MAGNET_COUNT);
    Serial.flush();
#else
    Serial.printf("[FOC] 轮速写死模式: %.1f km/h (协议值发 %d) — 装好磁钢后把 USE_HALL_SPEED 改 1\n",
        HARDCODE_SPEED_DM / 10.0,
        calcFOCSpeedFromKMH(HARDCODE_SPEED_DM / 10.0f));
    Serial.flush();
#endif

    u8g2.begin();
    Serial.print("[OLED] 初始化完成\n"); Serial.flush();

    // 复位原因 —— 车上要是莫名其妙重启, 这一行直接告诉我们是棕断还是看门狗
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   g_rst_reason = "POWERON"; break;
        case ESP_RST_EXT:       g_rst_reason = "EXT"; break;
        case ESP_RST_SW:        g_rst_reason = "SW"; break;
        case ESP_RST_PANIC:     g_rst_reason = "PANIC"; break;
        case ESP_RST_INT_WDT:   g_rst_reason = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:  g_rst_reason = "TASK_WDT"; break;
        case ESP_RST_WDT:       g_rst_reason = "WDT"; break;
        case ESP_RST_BROWNOUT:  g_rst_reason = "BROWNOUT"; break;
        case ESP_RST_DEEPSLEEP: g_rst_reason = "DEEPSLEEP"; break;
        default:                g_rst_reason = "UNKNOWN"; break;
    }
    g_boot_ms = millis();
    Serial.printf("[BOOT] 复位原因: %s\n", g_rst_reason); Serial.flush();

    // 显示任务 —— 独立跑, 不占 485 应答的时间
    disp_lock = xSemaphoreCreateMutex();
    xTaskCreate(oledTask, "oled", OLED_TASK_STACK, nullptr, OLED_TASK_PRIO, nullptr);

    Serial.printf("[MEM] free=%d\n", esp_get_free_heap_size()); Serial.flush();

#if OTA_AP_ENABLE
    // 放最后: WiFi 初始化要 100ms 左右, 别拖慢 485 起步
    otaService.begin();
#endif

#if BLE_ENABLE
    // 再放最后: bluedroid 启动要几百 ms + 几十 KB 堆, 绝不能拖慢 485 起步.
    // 失败也无所谓 —— 485 和 OTA 都不依赖 BLE.
    bleTelemetry.begin();
#if OTA_AP_ENABLE
    bleTelemetry.setApToggle(&bleApToggle);
#endif
    bleTelemetry.setVersionProvider(&bleVersionProvider);
    Serial.printf("[MEM] BLE 之后 free=%d\n", esp_get_free_heap_size()); Serial.flush();
#endif

    Serial.print("[READY] 等待 V35 ECU 轮询\n"); Serial.flush();
}

void loop() {
#if DEBUG_INJECT_485
    // 调试用: 在调试口敲 "[TX] 68 31 CE ... 16" 回车, 把字节喂进 485 解析链路.
    // 没有车的时候也能验 BMS 解析 —— 但只认 [TX] 开头的行, 免得误敲的字符被当成帧.
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            injectBuf[injectLen] = 0;
            if (strncmp(injectBuf, "[TX]", 4) == 0) {
                uint8_t tmp[128];
                size_t n = 0;
                for (size_t i = 4; i < injectLen && n < sizeof(tmp); i++) {
                    char ch = injectBuf[i];
                    int hi = -1;
                    if (ch >= '0' && ch <= '9') hi = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
                    if (hi < 0) continue;
                    if (injectHexPos == 0) { injectHi = hi; injectHexPos = 1; continue; }
                    injectHexPos = 0;
                    tmp[n++] = (uint8_t)((injectHi << 4) | hi);
                }
                for (size_t i = 0; i < n; i++) rxPush(tmp[i]);
                Serial.printf("[注入] %d 字节\n", (int)n);
            }
            injectLen = 0;
            injectHexPos = 0;
            Serial.flush();
        } else if (injectLen < sizeof(injectBuf) - 1) {
            injectBuf[injectLen++] = (char)c;
        }
    }
#endif

    while (Serial1.available()) {
        int c = Serial1.read();
        if (c >= 0) rxPush((uint8_t)c);
    }

    updateWheelSpeed();

#if OTA_AP_ENABLE
    // HTTP 收包在主循环里跑, 4KB 一块只阻塞几 ms, 不影响 485 的 200ms 轮询
    otaService.handle();
#endif

#if BLE_ENABLE
    bleTelemetry.publish({
        .speed_dm = foc_status.wheel_speed_dm,
        .rpm = foc_status.wheel_rpm,
        .voltage = foc_status.voltage,
        .current = foc_status.current,
        .protocol_speed = (uint8_t)foc_status.foc_speed,
        .error = foc_status.error,
        .unlocked = foc_status.unlocked,
        .glitch_count = (uint8_t)((hall_glitch_count > 255) ? 255 : hall_glitch_count),
        .q_count = simulator.q_count,
        .a_count = simulator.a_count,
        .pulse_count = (uint16_t)wheel_pulse_count,
        .bad_cs = (uint8_t)simulator.bad_cs,
        .interval_x10 = (uint8_t)(last_interval_us / 10000 > 255 ? 255 : last_interval_us / 10000),
    });
    // BMS 诊断计数 —— 小程序上"没电池数据"时, 用这几个数定位是哪一环断了.
    // 5Hz 往蓝牙任务里塞就够 (通知本身也是 1Hz), 每次循环都塞只是白占互斥量.
    static unsigned long last_bms_pub = 0;
    if (millis() - last_bms_pub > 200) {
        last_bms_pub = millis();
        bleTelemetry.publishBattery(bms_info);
        bms_diag.ageMs = bms_seen ? (millis() - bms_last_rx_ms) : 0xFFFFFFFFu;
        bms_diag.qCount = simulator.q_count;
        bms_diag.aCount = simulator.a_count;
        bleTelemetry.publishDiag(bms_diag);
    }
    bleTelemetry.handle();
#endif

    // 电池状态每秒打一行 —— 车上没接 BLE 时也能在串口里看到
    if (bms_info.hasStatus && millis() - bms_last_log > 1000) {
        bms_last_log = millis();
        Serial.printf("[BMS] %.1fV  %dA  电量 %d%%  温度 %d/%d/%d/%d/%d",
            bms_info.voltage_dv / 10.0f, bms_info.current, bms_info.soc,
            bms_info.temps[0], bms_info.temps[1], bms_info.temps[2],
            bms_info.temps[3], bms_info.temps[4]);
        if (bms_info.hasCells) {
            uint16_t minMv = 0xFFFF, maxMv = 0;
            for (uint8_t i = 0; i < bms_info.cellCount; i++) {
                if (bms_info.cellMv[i] < minMv) minMv = bms_info.cellMv[i];
                if (bms_info.cellMv[i] > maxMv) maxMv = bms_info.cellMv[i];
            }
            Serial.printf("  单体 %d 串 %.3f~%.3fV", bms_info.cellCount,
                minMv / 1000.0f, maxMv / 1000.0f);
        }
        Serial.printf("\n");
    }

#if TEST_485_ENABLE
    // 每秒主动发一帧测试数据 — 确认 ESP32 确实在往 485 总线上发
    static unsigned long lastTest485 = 0;
    if (millis() - lastTest485 > TEST_485_INTERVAL_MS) {
        lastTest485 = millis();
        simulator.sendTestFrame();
    }
#endif

    // 板载 LED 闪烁 — 肉眼确认程序活着
    static unsigned long lastLedToggle = 0;
    if (millis() - lastLedToggle > 500) {
        lastLedToggle = millis();
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }

    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 100) {
        lastOledUpdate = millis();
        // 只更新快照, 画屏交给 OLED 任务 —— 这里绝不能被 25ms 的 I2C 传输堵住
        DispState s = {
            foc_status.wheel_speed_dm,
            foc_status.wheel_rpm,
            foc_status.error,
            bms_info.hasStatus ? (uint8_t)bms_info.soc : (uint8_t)0xFF,
            (uint8_t)foc_status.unlocked,
        };
        if (disp_lock && xSemaphoreTake(disp_lock, portMAX_DELAY) == pdTRUE) {
            disp_state = s;
            xSemaphoreGive(disp_lock);
        }
    }

    if (millis() - last_debug > 3000) {
#if USE_HALL_SPEED
        Serial.printf("[状态] %s | 轮速:%dRPM | %.1fkm/h | 间隔:%lums(中位%lums) | 脉冲:%lu | 毛刺:%lu | 停车重置:%lu\n",
            foc_status.unlocked ? "已解锁" : "已锁定",
            foc_status.wheel_rpm,
            foc_status.wheel_speed_dm / 10.0,
            (unsigned long)last_interval_us,
            (unsigned long)hall_median_us,
            (unsigned long)wheel_pulse_count,
            (unsigned long)hall_glitch_count,
            (unsigned long)hall_gap_reset);
#else
        Serial.printf("[状态] %s | 轮速:%dRPM | %.1fkm/h\n",
            foc_status.unlocked ? "已解锁" : "已锁定",
            foc_status.wheel_rpm,
            foc_status.wheel_speed_dm / 10.0);
#endif
        // 485 侧的对答情况 —— 排查"仪表没速度"时先看这两个数:
        //   查询次数应该一直涨 (涨=总线通), 应答次数要跟查询次数一致
        Serial.printf("[485] 查询:%u 应答:%u 其它:%u 校验错:%u | 协议车速:%u\n",
            simulator.q_count, simulator.a_count, simulator.oth, simulator.bad_cs,
            foc_status.foc_speed);
        // BMS 侧 —— "小程序没电池数据"时先看这一行:
        //   帧数=0 → 总线上就没有 BMS 应答; 帧数>0 但成功=0 → 校验和/长度有问题 (被干扰)
        Serial.printf("[BMS] 帧:%u 成功:%u 校验错:%u 长度错:%u 未知:%u 距上次:%lums\n",
            bms_diag.rxFrames, bms_diag.okFrames, bms_diag.csErr, bms_diag.lenErr,
            bms_diag.unknownLen, (long)(bms_seen ? (millis() - bms_last_rx_ms) : -1));
        Serial.printf("[LOG] 485 日志缓冲 %u 帧\n", (unsigned)busLog().count());
#if OTA_AP_ENABLE
        // 热点状态 —— 排查"手机连不上热点"时看这一行:
        // 客户端数一直是 0 = 手机压根没关联上 (不是 HTTP 层的问题)
        Serial.printf("[AP] %s 信道%d 客户端:%d\n",
            OTA_AP_SSID, (int)WiFi.channel(), (int)WiFi.softAPgetStationNum());
#endif
        // 刻意不 flush —— 电脑接上但没人读时, 串口 TX 缓冲会堵住主循环,
        // 而 V35 的轮询超时只有 ~20ms, 卡一次这一轮就变成"没应答→错误码190→车速0"
        last_debug = millis();
    }
}
