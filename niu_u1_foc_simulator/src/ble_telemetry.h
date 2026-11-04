/**
 * BLE 遥测 — 把 485 总线上的车辆状态推给微信小程序
 *
 * 为什么单独开一条 BLE 通道:
 *   OTA 走 WiFi 热点, 手机一连热点就断网, 推送完还得手动切回去;
 *   看车速不需要断网, BLE 直连更顺手. 两条通道互不干扰, 可以同时开着.
 *
 * GATT 结构:
 *   Service  6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *     ├─ 6e400002  notify  车辆状态, 20 字节小端 (见 BleTelemetryData)
 *     ├─ 6e400003  notify  485 总线上抓到的原始帧 (最新一帧的字节)
 *     └─ 6e400004  notify  电池信息 (BMS 解析结果, 消息分片格式, 见下面)
 *
 * 电池信息的消息格式 —— 一条特征上跑多种消息, 每条 notify 带一个 3 字节消息头:
 *
 *   [0] 消息类型   1=状态 2=单体电压 3=型号串
 *   [1] 分片序号   从 0 开始
 *   [2] 分片总数
 *   [3..] 数据
 *
 *   状态 (17 字节): flags | 电量% | 总电压0.1V | 电流0.1A | 温度个数 | 5×温度 |
 *                    单体串数 | 最低单体mV | 最高单体mV
 *   单体电压: 每格 2 字节 mV (小端), 一片最多带 8 格
 *   型号串: 16 字节 ASCII
 *
 * 分片是因为协商不出 MTU 时单条 notify 只能发 20 字节, 而单体电压 13 串就有 26 字节.
 *
 * 注意事项:
 *   1) Arduino-ESP32 2.0.x 自带的 BLE 库在 C3 上能编能跑 (Bluedroid, 无经典蓝牙),
 *      但它吃内存 —— bluedroid 初始化要几十 KB 堆. 所以 begin() 放 setup 最后,
 *      失败也不能影响 485.
 *   2) notify 的长度上限由协商出来的 MTU 决定 (最小 20), 帧长按 BLE_MAX_NOTIFY_LEN 切块.
 */

#ifndef BLE_TELEMETRY_H
#define BLE_TELEMETRY_H

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "ble_config.h"
#include "bms_protocol.h"
#include "ble_battery_msg.h"

// 遥测载荷: 小端, 20 字节
struct BleTelemetryData {
    uint16_t speed_dm;        // 车速 ×10 (十分位)
    uint16_t rpm;             // 轮速转速
    uint16_t voltage;         // 电压 (0.1V)
    int16_t  current;         // 电流 (0.1A)
    uint8_t  protocol_speed;  // 485 协议车速值 (0-127)
    uint8_t  error;           // 错误码
    uint8_t  unlocked;        // 1 = 已解锁
    // 字节 11 原来放的是 flags (bit0 = 刚收到过 485 帧), 但那个位设了从来没读过,
    // 小程序也没显示 —— 干脆拿来传毛刺计数, 诊断车速问题用.
    // 不加新字段是为了不动 20 字节的帧长: 协商不出 MTU 时单条 notify 只能发 20 字节,
    // 加到 24 字节在有些手机上会直接发不出去, 整条遥测就断了.
    uint8_t  glitch_count;    // 被判为毛刺丢掉的快脉冲数 (饱和到 255)
    // ↓ 诊断字段: 骑车时用小程序一眼分清"是霍尔没脉冲"还是"485 没应答"
    uint16_t q_count;         // 收到 V35 状态查询的次数
    uint16_t a_count;         // 我们回出去的应答次数 (应与 q_count 相等)
    uint16_t pulse_count;     // 霍尔脉冲累计数 (低 16 位)
    uint8_t  bad_cs;          // 校验和不对的帧数 (总线干扰)
    uint8_t  interval_x10;    // 最近一次脉冲间隔 (10ms 单位)
};

class BleCommand {
public:
    // 命令集 —— 单字节, 大写字母好认 (小程序日志里能直接看到)
    static const uint8_t CMD_START_AP  = 'W';   // 临时开 OTA 热点 (推固件用)
    static const uint8_t CMD_STOP_AP   = 'X';   // 关热点, 射频还给蓝牙
    static const uint8_t CMD_VERSION   = 'V';   // 问固件版本号
    // 回执状态码
    static const uint8_t REPLY_DONE      = 1;
    static const uint8_t REPLY_NOSUPPORT = 0xFF;
};

class BleTelemetry;

// 命令特征的回调 —— 只记下「收到了什么」, WiFi 开关由主循环执行
// (BLE 回调跑在蓝牙任务里, 在那边动 WiFi 会跟共存调度打架, 必须延后)
class BleCmdCb : public BLECharacteristicCallbacks {
public:
    explicit BleCmdCb(BleTelemetry* owner) : owner_(owner) {}
    // 必须用带 param 的重载: 废弃版 onWrite(pCharacteristic) 里 getValue() 拿到的
    // 不一定是刚写进来的字节 (实测写 0x57 读回来是 0x00). param->write 才是原始数据.
    void onWrite(BLECharacteristic* ch, esp_ble_gatts_cb_param_t* param) override;
private:
    BleTelemetry* owner_;
};

class BleTelemetry {
public:
    BleTelemetry() : serverCb_(this), cmdCb_(this) {}

    // BLE 初始化 (bluedroid 启动, 几百 ms). 失败返回 false, 不影响 485 主功能
    bool begin() {
        BLEDevice::init(BLE_DEVICE_NAME);
        BLEDevice::setMTU(185);

        BLEServer* server = BLEDevice::createServer();
        server->setCallbacks(&serverCb_);

        BLEService* svc = server->createService(BLE_SERVICE_UUID);

        telemetry_ = svc->createCharacteristic(BLE_TELEMETRY_UUID,
                                               BLECharacteristic::PROPERTY_NOTIFY |
                                               BLECharacteristic::PROPERTY_READ);
        telemetry_->addDescriptor(new BLE2902());

        frame_ = svc->createCharacteristic(BLE_FRAME_UUID,
                                           BLECharacteristic::PROPERTY_NOTIFY |
                                           BLECharacteristic::PROPERTY_READ);
        frame_->addDescriptor(new BLE2902());

        battery_ = svc->createCharacteristic(BLE_BATTERY_UUID,
                                             BLECharacteristic::PROPERTY_NOTIFY |
                                             BLECharacteristic::PROPERTY_READ);
        battery_->addDescriptor(new BLE2902());

        cmdChar_ = svc->createCharacteristic(BLE_CMD_UUID,
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_NOTIFY);
        cmdChar_->addDescriptor(new BLE2902());
        cmdChar_->setCallbacks(&cmdCb_);

        svc->start();

        // 广播包布局很关键 (踩过坑):
        //   库默认行为是「128 位 UUID + 名字 + txpower 全塞扫描回应, 主广播包只有 Flags」
        //   —— 扫描回应只有手机主动发 SCAN_REQ 才收得到 (主动扫描). iPhone 后台的小程序
        //   用的是被动扫描, 收不到扫描回应, 看到的就是"一个没名字的设备", 直接被名字
        //   过滤掉了. 所以必须用自定义广播包: 名字放主广播包 (所有扫描都收得到),
        //   128 位 UUID 放扫描回应 (连接前用不上, 连接时靠逐个特征枚举).
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        // 广播间隔: 单位 0.625ms, 160/240 = 100~150ms 一发 (实测 ~8 个/秒).
        // 库默认 20~40ms 更快, 但那是在纯蓝牙下测的; 留点余量给 WiFi 共存调度.
        adv->setMinInterval(160);   // 100ms
        adv->setMaxInterval(240);   // 150ms
        BLEAdvertisementData advData;
        advData.setFlags(0x06);                       // LE General Discovery + BR/EDR 不支持
        advData.setName(BLE_DEVICE_NAME);             // 名字必须在主广播包里!
        adv->setAdvertisementData(advData);

        BLEAdvertisementData scanRsp;
        // 扫描回应里放名字 + 128 位 UUID (不带 Flags: 名字 12 + UUID 18 = 30 字节,
        // 刚好在 31 字节上限内; 加 Flags 就超了, addData 会静默丢弃).
        // 带上 UUID 是为了让"按服务 UUID 过滤"的扫描器也能匹配上.
        scanRsp.setName(BLE_DEVICE_NAME);
        scanRsp.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
        adv->setScanResponseData(scanRsp);
        adv->setScanResponse(false);   // 不再让库自动拼扫描回应, 上面那份就是
        adv->start();

        connected_ = false;
        Serial.printf("[BLE] 已启动 %s, 剩余堆 %u 字节\n",
                      BLE_DEVICE_NAME, (unsigned)ESP.getFreeHeap());
        return true;
    }

    bool connected() const { return connected_; }

    // 存一份最新状态, 由 handle() 按 BLE_NOTIFY_PERIOD_MS 推给手机
    void publish(const BleTelemetryData& d) { data_ = d; }

    // 485 总线上的原始帧 —— 让小程序能看到 ECU 到底在问什么
    void pushFrame(const uint8_t* raw, size_t len) {
        if (!raw || len == 0) return;
        if (len > sizeof(frameBuf_)) len = sizeof(frameBuf_);
        memcpy(frameBuf_, raw, len);
        frameLen_ = len;
        frameSeq_++;
    }

    // 电池信息 —— BMS 应答解析出来的结果
    void publishBattery(const BmsInfo& info) { bms_ = info; }

    // ---- WiFi 热点遥控 (蓝牙优先方案的核心) ----

    // 命令回调只记下命令, 主循环 handle() 里执行, 回统一走 notifyReply()
    void queueCommand(uint8_t cmd) { cmdByte_ = cmd; cmdPending_ = true; }

    // 命令回执通知: [0]=命令 [1]=状态 (0=收到 1=已执行 0xFF=不认识)
    void notifyReply(uint8_t cmd, uint8_t state) {
        if (!cmdChar_ || !connected_) return;
        uint8_t b[2] = { cmd, state };
        cmdChar_->setValue(b, sizeof(b));
        cmdChar_->notify();
    }

    // 版本号回执: [0]='V' [1..]=版本字符串 (ASCII, 不含结尾 0)
    // 小程序在车辆页显示这个, 不用连 WiFi 就能知道板上跑的是哪一版
    void notifyVersion(const char* ver) {
        if (!cmdChar_ || !connected_ || !ver) return;
        size_t n = strlen(ver);
        if (n > 18) n = 18;                    // 20 字节通知里留 2 字节头
        uint8_t b[20];
        b[0] = BleCommand::CMD_VERSION;
        memcpy(b + 1, ver, n);
        cmdChar_->setValue(b, n + 1);
        cmdChar_->notify();
    }

    // 命令执行的钩子 —— 主程序 (.ino) 注入真正的 WiFi 开关实现, 该头不依赖 ota_server.h
    typedef void (*ApToggleFn)(bool on);
    void setApToggle(ApToggleFn fn) { apToggle_ = fn; }

    // 版本号的提供者 —— 主程序 (.ino) 注入 (ota_config.h 里的 OTA_FW_VERSION)
    typedef void (*VersionFn)(char* out, size_t cap);
    void setVersionProvider(VersionFn fn) { versionFn_ = fn; }

    bool cmdPending() const { return cmdPending_; }


    // 主循环里调, 不阻塞
    void handle() {
        unsigned long now = millis();

        // 执行蓝牙命令队列里的 WiFi 开关 —— 必须延后到主循环:
        // BLE 回调跑在蓝牙任务里, 在那边动 WiFi 会跟共存调度打架
        if (cmdPending_) {
            uint8_t cmd = cmdByte_;
            cmdPending_ = false;
            if (cmd == BleCommand::CMD_VERSION) {
                if (versionFn_) {
                    char ver[20] = {0};
                    versionFn_(ver, sizeof(ver));
                    notifyVersion(ver);
                } else {
                    notifyReply(cmd, BleCommand::REPLY_NOSUPPORT);
                }
            } else if (apToggle_) {
                apToggle_(cmd == BleCommand::CMD_START_AP);
                notifyReply(cmd, BleCommand::REPLY_DONE);
            } else {
                notifyReply(cmd, BleCommand::REPLY_NOSUPPORT);
            }
        }

        // 断了以后要重新广播. 关键: 不能在 BLE 的回调里直接调 advertising->start() ——
        // 那个回调跑在蓝牙任务里, 这时候 GAP 状态机还没处理完断连, 启动广播会被
        // 静默丢掉 (设备就再也不广播了, 手机再也扫不到). 必须延后到主循环再启动.
        if (advRestartAt_ && (int32_t)(now - advRestartAt_) >= 0) {
            BLEDevice::getAdvertising()->start();
            advRestartAt_ = 0;
            lastDisconnectAt_ = now;
            Serial.println("[BLE] 重新广播");
        } else if (!connected_ && (now - lastAdvCheckAt_) > BLE_REBROADCAST_PERIOD_MS) {
            // 兜底: 定期重启广播.
            // 为什么必须这么做: 上面的"重新广播"只在断连回调触发过一次, 而那次
            // advertising->start() 有可能正好撞上 GAP 状态机没准备好而被静默丢掉 ——
            // 代码却以为成功了, 于是设备从此再也不广播 (板子活着, 485 正常, 但手机扫不到).
            // 这种状态在车上没法自查, 只能靠定期重来一遍保证广播一定活着.
            BLEDevice::getAdvertising()->start();
            lastAdvCheckAt_ = now;
            lastDisconnectAt_ = now;
            Serial.println("[BLE] 广播重启 (定期兜底)");
        }

        if (!connected_) return;
        if (now - lastNotify_ < BLE_NOTIFY_PERIOD_MS) return;
        lastNotify_ = now;

        uint8_t buf[20];
        buf[0]  = data_.speed_dm & 0xFF;
        buf[1]  = data_.speed_dm >> 8;
        buf[2]  = data_.rpm & 0xFF;
        buf[3]  = data_.rpm >> 8;
        buf[4]  = data_.voltage & 0xFF;
        buf[5]  = data_.voltage >> 8;
        buf[6]  = data_.current & 0xFF;
        buf[7]  = (uint16_t)(data_.current >> 8) & 0xFF;
        buf[8]  = data_.protocol_speed;
        buf[9]  = data_.error;
        buf[10] = data_.unlocked;
        buf[11] = data_.glitch_count;
        buf[12] = data_.q_count & 0xFF;
        buf[13] = data_.q_count >> 8;
        buf[14] = data_.a_count & 0xFF;
        buf[15] = data_.a_count >> 8;
        buf[16] = data_.pulse_count & 0xFF;
        buf[17] = data_.pulse_count >> 8;
        buf[18] = data_.bad_cs;
        buf[19] = data_.interval_x10;
        telemetry_->setValue(buf, sizeof(buf));
        telemetry_->notify();

        // 有新帧才推, 否则手机侧不用被无意义的通知打扰
        if (frameSeq_ != lastFrameSeq_) {
            lastFrameSeq_ = frameSeq_;
            size_t off = 0;
            while (off < frameLen_) {
                size_t n = frameLen_ - off;
                if (n > BLE_MAX_NOTIFY_LEN) n = BLE_MAX_NOTIFY_LEN;
                frame_->setValue((uint8_t*)frameBuf_ + off, n);
                frame_->notify();
                off += n;
            }
        }

        sendBattery();
    }

private:
    // 一条电池消息按 BLE_BMSG_PAYLOAD_MAX 切块, 每块带 [类型][片号][总片数] 头
    void sendBatteryMsg(uint8_t type, const uint8_t* payload, size_t len) {
        size_t total = (len + BLE_BMSG_PAYLOAD_MAX - 1) / BLE_BMSG_PAYLOAD_MAX;
        if (total == 0) return;
        uint8_t chunk[BLE_BMSG_CHUNK_MAX];
        for (size_t i = 0; i < total; i++) {
            size_t n = bleBatteryPackChunk(type, payload, len, (uint8_t)i, chunk);
            if (n == 0) break;
            battery_->setValue(chunk, n);
            battery_->notify();
        }
    }

    static uint16_t hashBytes(const uint8_t* p, size_t n) {
        uint16_t h = 0x1D0F;
        for (size_t i = 0; i < n; i++) h = (uint16_t)((h * 31) ^ p[i]);
        return h;
    }

    void sendBattery() {
        if (bms_.hasStatus) {
            uint8_t p[BLE_BMSG_STATUS_LEN];
            size_t n = bleBatteryPackStatus(&bms_, p);
            sendBatteryMsg(BLE_BMSG_STATUS, p, n);
        }

        // 单体电压: 变了才推 (13 串 = 26 字节, 要分 2 片)
        if (bms_.hasCells) {
            uint16_t h = hashBytes((const uint8_t*)bms_.cellMv, bms_.cellCount * 2);
            if (h != lastCellsHash_) {
                lastCellsHash_ = h;
                uint8_t p[BMS_MAX_CELLS * 2];
                size_t n = bleBatteryPackCells(&bms_, p);
                sendBatteryMsg(BLE_BMSG_CELLS, p, n);
            }
        }

        // 型号串: 变了才推
        if (bms_.hasModel) {
            uint16_t h = hashBytes((const uint8_t*)bms_.model, strlen(bms_.model));
            if (h != lastModelHash_) {
                lastModelHash_ = h;
                uint8_t p[BMS_MODEL_CHARS];
                size_t n = bleBatteryPackModel(&bms_, p);
                sendBatteryMsg(BLE_BMSG_MODEL, p, n);
            }
        }
    }

private:
    class ServerCb : public BLEServerCallbacks {
    public:
        explicit ServerCb(BleTelemetry* owner) : owner_(owner) {}
        void onConnect(BLEServer* s) override {
            owner_->handleConnect(s);
        }
        void onDisconnect(BLEServer* s) override {
            owner_->handleDisconnect(s);
        }
    private:
        BleTelemetry* owner_;
    };

    void handleConnect(BLEServer* s) {
        connected_ = true;
        Serial.println("[BLE] 手机已连接");
        (void)s;
    }

    void handleDisconnect(BLEServer* s) {
        (void)s;
        connected_ = false;
        // 单体电压/型号串是"变了才推", 但手机重连后小程序那边的状态是空的 ——
        // 不把哈希清掉的话, 重连后这两个消息永远不会再发, 电池面板只剩状态没有单体条
        lastCellsHash_ = 0;
        lastModelHash_ = 0;
        Serial.println("[BLE] 手机断开, 稍后重新广播");
        advRestartAt_ = millis() + 300;
    }

    ServerCb serverCb_;
    BleCmdCb cmdCb_;
    BLECharacteristic* telemetry_ = nullptr;
    BLECharacteristic* frame_ = nullptr;
    BLECharacteristic* battery_ = nullptr;
    BLECharacteristic* cmdChar_ = nullptr;
    uint8_t cmdByte_ = 0;
    bool cmdPending_ = false;
    ApToggleFn apToggle_ = nullptr;
    VersionFn versionFn_ = nullptr;
    BleTelemetryData data_ = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    BmsInfo bms_ = {0};
    uint16_t lastCellsHash_ = 0;
    uint16_t lastModelHash_ = 0;
    bool connected_ = false;
    unsigned long lastNotify_ = 0;
    unsigned long advRestartAt_ = 0;
    unsigned long lastDisconnectAt_ = 0;
    unsigned long lastAdvCheckAt_ = 0;
    uint8_t frameBuf_[64];
    size_t frameLen_ = 0;
    uint16_t frameSeq_ = 0;
    uint16_t lastFrameSeq_ = 0;
};

// 命令特征写入回调 —— 只排队, 不执行 (WiFi 开关必须回主循环)
inline void BleCmdCb::onWrite(BLECharacteristic* ch, esp_ble_gatts_cb_param_t* param) {
    if (!owner_ || !param) return;
    uint16_t len = param->write.len;
    if (len == 0) return;
    // 取最后一个字节: 小程序写 1 字节就是它自己; 而 gatttool 之类的工具写 "0x57"
    // 会被当成 2 字节大端 (00 57), 取头就拿到 0x00 了.
    uint8_t cmd = param->write.value[len - 1];
    owner_->queueCommand(cmd);
    Serial.printf("[BLE] 命令 0x%02X 已排队\n", cmd);
}

#endif
