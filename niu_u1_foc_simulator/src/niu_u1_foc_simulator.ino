/**
 * 小牛U1 FOC电机控制器模拟器 - ESP32-C3
 * 485 总线响应 V35 ECU 轮询 + 霍尔轮速 + OLED 速度显示
 */

#include <HardwareSerial.h>
#include <esp_log.h>
#include <U8g2lib.h>
#include <Wire.h>

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
// 0 = 写死车速 (霍尔还没装好, 用固定值喂给 V35 ECU 调试通信链路)
// 1 = 霍尔实测 (磁钢盘装好后改成 1)
#define USE_HALL_SPEED    0
// 写死的车速, 单位 km/h × 10 (170 = 17.0 km/h)
// 17 km/h 超出协议对照表范围, 对应协议值 31 (封顶), 仪表显示约 17 km/h
#define HARDCODE_SPEED_DM 170

// ============ 联调测试开关 ============
// 485 每秒主动发一帧测试数据, 用来确认 ESP32 确实在往总线上发.
// 观察方式:
//   - USB-485 转换器接上 → 串口工具里能看到周期性字节
//   - 485 模块上的 TX 指示灯会跟着闪
// 联调完记得改回 0 —— 测试帧是协议里没有的功能码, 会干扰 V35 ECU 的正常轮询.
#define TEST_485_ENABLE      0
#define TEST_485_INTERVAL_MS 1000

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

// ============ 轮速测量 ============
volatile uint32_t wheel_pulse_count = 0;
volatile uint32_t last_pulse_us = 0;

// 协议速度值 (0-31) 分档映射 — 抓包反推出来的非线性表, 不能用线性换算
// 输入: 实际车速 (整数 km/h), 输出: 协议值
//   0→0  1→3  2→5  3→7  4→10  5/6→13  7/8→17  9/10→21  11/12→25  13/14→30  15+→31
uint16_t calcFOCSpeedFromKMH(uint16_t kmh) {
    if (kmh <= 0) return 0;
    if (kmh <= 1) return 3;
    if (kmh <= 2) return 5;
    if (kmh <= 3) return 7;
    if (kmh <= 4) return 10;
    if (kmh <= 6) return 13;
    if (kmh <= 8) return 17;
    if (kmh <= 10) return 21;
    if (kmh <= 12) return 25;
    if (kmh <= 14) return 30;
    return 31;
}

// 轮速更新由 loop 周期驱动 (不是只在收到查询时), OLED 才能显示实时速度
static unsigned long lastSpeedCalc = 0;
static uint32_t lastPulseSnapshot = 0;

void updateWheelSpeed() {
    unsigned long now = millis();
    unsigned long dt = now - lastSpeedCalc;
    if (dt < 250) return;
    lastSpeedCalc = now;

#if USE_HALL_SPEED
    float secs = dt / 1000.0f;
    uint32_t pulses = wheel_pulse_count - lastPulseSnapshot;
    lastPulseSnapshot = wheel_pulse_count;
    float rpm = (pulses * 60.0f) / (MAGNET_COUNT * secs);
    float kmh = rpm * WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;
#else
    // 霍尔未安装: 直接给车速, 后面整条链路 (显示 + 协议值) 逻辑不变
    float kmh = HARDCODE_SPEED_DM / 10.0f;
    float rpm = kmh * 1000.0f / (WHEEL_CIRCUMFERENCE_M * 60.0f);
#endif

    foc_status.wheel_rpm = (uint16_t)rpm;
    foc_status.wheel_speed_dm = (uint16_t)(kmh * 10.0f);
    foc_status.foc_speed = calcFOCSpeedFromKMH((uint16_t)kmh);
}

void IRAM_ATTR wheelPulseISR() {
    uint32_t now = micros();
    // 1ms 消抖: 单磁钢在 30km/h 下脉冲间隔约 154ms, 1ms 阈值不会漏计真实脉冲
    if (now - last_pulse_us > 1000) {
        wheel_pulse_count++;
        last_pulse_us = now;
    }
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
//   [9]    车速协议值 (0-31, int8; 负数=倒挂). ECU 换算: 仪表速度 = 值*12/25
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
        setTransmitMode(true);
        serial->write(frame, idx);
        serial->flush();
        // 调试回显：镜像到 USB CDC (独立通道，不污染 485)
        Serial.printf("[485→] ");
        for (size_t i = 0; i < idx; i++) Serial.printf("%02X ", frame[i]);
        Serial.printf("\n");
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
        foc_response_data[9] = (uint8_t)foc_status.foc_speed;   // 车速协议值 (0-31)
        foc_response_data[10] = (foc_status.foc_speed == 0) ? FOC_INFO_FLAG_1_PARKING : 0x00;
        foc_response_data[11] = 0x00;
        if (extended) {
            foc_response_data[12] = 0x00;
            foc_response_data[13] = 0x02;
        }
    }

public:
    FOCSimulator(HardwareSerial* s) : serial(s) {}

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
            sendFrame(FUNC_RESPONSE, foc_response_data, 12);
        } else if (data[0] == 0x05 && data[1] == 0x09) {
            updateFOCResponse(true);
            sendFrame(FUNC_RESPONSE, foc_response_data, 14);
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
                Serial.printf("[V35] unknown control: 0x%02X len=%d\n", data[0], len);
                Serial.flush();
                break;
        }
    }

    void handleSetParam(const uint8_t* data, uint8_t len) {
        uint8_t ack_data[] = { 0xCE };
        sendFrame(FUNC_SET_PARAM_ACK, ack_data, 1);
    }

    void handleWriteParam(const uint8_t* data, uint8_t len) {
        // V35 数据写入 (0x04) — 只 ACK
        Serial.printf("[V35] FUNC_WRITE_PARAM: %d bytes\n", len);
        Serial.flush();
        uint8_t ack_data[] = { 0xCD };
        sendFrame(FUNC_WRITE_PARAM_ACK, ack_data, 1);
    }

    void handleConfigSystem(const uint8_t* data, uint8_t len) {
        // V35 系统配置 (0x06) — 主/从模式等
        uint8_t subcmd = len > 0 ? data[0] : 0xFF;
        const char* mode = "?";
        switch (subcmd) {
            case 0x01: mode = "set_slave"; break;
            case 0x02: mode = "set_master"; break;
            default:    mode = "other";    break;
        }
        Serial.printf("[V35] FUNC_CONFIG_SYSTEM: subcmd=0x%02X (%s)\n", subcmd, mode);
        Serial.flush();
        // 原厂 FOC 对 0x06 的确认是空数据 (抓包: 68 20 DF 68 86 00 CS 16)
        static const uint8_t empty[] = {0x00};
        sendFrame(FUNC_CONFIG_SYSTEM_ACK, empty, 0);
    }

    void parseFrame(const uint8_t* raw_frame, size_t len) {
        if (len < 9 || raw_frame[0] != 0x68 || raw_frame[len-1] != 0x16) return;
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
        uint8_t data[32] = {0};
        uint8_t actual_len = min(data_len, (uint8_t)32);
        for (uint8_t i = 0; i < actual_len; i++) {
            data[i] = raw_frame[6 + i] - 0x33;   // 数据区 -0x33 反混淆
        }
        switch (func & 0x7F) {
            case FUNC_QUERY:           handleQuery(data, actual_len); break;
            case FUNC_CONTROL:         handleControl(data, actual_len); break;
            case FUNC_SET_PARAM:       handleSetParam(data, actual_len); break;
            case FUNC_WRITE_PARAM:     handleWriteParam(data, actual_len); break;
            case FUNC_CONFIG_SYSTEM:   handleConfigSystem(data, actual_len); break;
        }
    }
};

FOCSimulator simulator(&Serial1);

static uint8_t rx_buf[256];
static size_t rx_len = 0;
static unsigned long last_debug = 0;

void setup() {
    disableLoopWDT();

    // USB CDC (调试日志) - platformio.ini 里 CDC_ON_BOOT=1
    // 独立于 485 总线, 调试 ASCII 不会污染 V35 总线
    Serial.begin(115200);
    delay(100);  // 等 USB 枚举

    Serial.print("=== 小牛U1 FOC模拟器 ===\n"); Serial.flush();

    // Wire 必须最早初始化 — 让 FreeRTOS mutex 和 ESP-IDF I2C0 driver 在最早可用时就绪,
    // 否则 u8g2 首次传输时 I2C0 driver 还没就绪, 会刷 "could not acquire lock" +
    // "NULL TX buffer pointer" 错误 (u8g2 每页传输前都调 Wire.setClock()).
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

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
        MAGNET_COUNT, WHEEL_CIRCUMFERENCE_M); Serial.flush();
#else
    Serial.printf("[FOC] 轮速写死模式: %.1f km/h (协议值发 %d) — 霍尔装好后把 USE_HALL_SPEED 改 1\n",
        HARDCODE_SPEED_DM / 10.0,
        calcFOCSpeedFromKMH((uint16_t)(HARDCODE_SPEED_DM / 10)));
    Serial.flush();
#endif

    u8g2.begin();
    Serial.print("[OLED] 初始化完成\n"); Serial.flush();

    Serial.printf("[MEM] free=%d\n", esp_get_free_heap_size()); Serial.flush();
    Serial.print("[READY] 等待 V35 ECU 轮询\n"); Serial.flush();
}

// 只显示速度: 大字 km/h + 底部 RPM / 错误码
void drawOled() {
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);

    // 大字车速 (十分位整数 → "23.4")
    char spd[16];
    snprintf(spd, sizeof(spd), "%u.%u",
             foc_status.wheel_speed_dm / 10, foc_status.wheel_speed_dm % 10);
    u8g2.setFont(u8g2_font_ncenB14_te);
    int w = u8g2.getStrWidth(spd);
    u8g2.setCursor((128 - w) / 2, 34);
    u8g2.print(spd);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor((128 - w) / 2 + w + 4, 24);
    u8g2.print("km/h");

    // 底部: RPM + 错误码
    char bottom[32];
    snprintf(bottom, sizeof(bottom), "%d RPM", (int)foc_status.wheel_rpm);
    u8g2.setCursor(0, 55);
    u8g2.print(bottom);
    if (foc_status.error != 0) {
        char err[8];
        snprintf(err, sizeof(err), "ERR%02X", foc_status.error);
        w = u8g2.getStrWidth(err);
        u8g2.setCursor(128 - w, 55);
        u8g2.print(err);
    }

    u8g2.sendBuffer();
}

void loop() {
    while (Serial1.available()) {
        int c = Serial1.read();
        if (c >= 0) {
            if (rx_len == 0) Serial.printf("[485←] ");
            Serial.printf("%02X ", c);
            rx_buf[rx_len++] = (uint8_t)c;
            if (c == 0x16 && rx_len > 9) {
                Serial.printf("\n"); Serial.flush();  // 帧边界 flush
                simulator.parseFrame(rx_buf, rx_len);
                rx_len = 0;
            }
            if (rx_len >= sizeof(rx_buf)) {
                Serial.printf("[overflow]\n"); Serial.flush();
                rx_len = 0;
            }
        }
    }

    updateWheelSpeed();

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
    if (millis() - lastOledUpdate > 200) {
        drawOled();
        lastOledUpdate = millis();
    }

    if (millis() - last_debug > 3000) {
        Serial.printf("[状态] %s | 轮速:%dRPM | %.1fkm/h\n",
            foc_status.unlocked ? "已解锁" : "已锁定",
            foc_status.wheel_rpm,
            foc_status.wheel_speed_dm / 10.0);
        Serial.flush();
        last_debug = millis();
    }
}
