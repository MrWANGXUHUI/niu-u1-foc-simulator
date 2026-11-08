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
 *     ├─ 6e400004  notify  电池信息 (BMS 解析结果 + 诊断, 消息分片格式, 见下面)
 *     ├─ 6e400005  write   命令 (开关热点 / 问版本 / 取 485 日志)
 *     └─ 6e400006  notify  485 总线日志 (拉取用, 见 bus_log.h)
 *
 * 电池信息的消息格式 —— 一条特征上跑多种消息, 每条 notify 带一个 3 字节消息头:
 *
 *   [0] 消息类型   1=状态 2=单体电压 3=型号串 4=诊断
 *   [1] 分片序号   从 0 开始
 *   [2] 分片总数
 *   [3..] 数据
 *
 *   状态 (17 字节): flags | 电量% | 总电压0.1V | 电流0.1A | 温度个数 | 5×温度 |
 *                    单体串数 | 最低单体mV | 最高单体mV
 *   单体电压: 每格 2 字节 mV (小端), 一片最多带 8 格
 *   型号串: 16 字节 ASCII
 *   诊断 (16 字节): BMS 帧数/解析成功数/各类错误数/距上次帧多少毫秒 (见 ble_battery_msg.h)
 *
 * 分片是因为协商不出 MTU 时单条 notify 只能发 20 字节, 而单体电压 13 串就有 26 字节.
 *
 * ⚠️ **通知必须在独立任务里发** (2026-11-04 改, 关键):
 *   以前 notify 是在主循环里直接发的. 主循环同时也是 485 应答的路径, 而 V35 单次
 *   轮询的超时只有 ~20ms. 蓝牙通知一多 (遥测+485帧+电池, 每秒 30~40 包), 一次突发
 *   就能把应答拖过 20ms → V35 等不到 FOC 应答就跳过, 直接去问 BMS; 我们迟到的应答
 *   跟 BMS 的应答在总线上撞车, BMS 的帧被打烂 → **小程序上"电池没数据"**,
 *   而车速是好的 (车速那次通知是突发里的第一个, 总是发得出去).
 *   所以现在: 主循环只往快照里塞数据 (几十字节的 memcpy), 通知全部交给 BLE 任务.
 *   同 OLED 任务一样的思路 —— 慢活挪出主循环.
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

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "ble_config.h"
#include "bms_protocol.h"
#include "ble_battery_msg.h"
#include "bus_log.h"

// BLE 任务参数 —— 跟 OLED 任务一样, 优先级跟 Arduino 的 loop 任务同级
#define BLE_TASK_STACK     4096
#define BLE_TASK_PRIO      1
// BLE 任务主循环的歇脚时间 (ms). 通知的节奏由各自的周期控制, 这个只是别把 CPU 占满
#define BLE_TASK_TICK_MS   5
// 电池状态推送周期 (ms). 电量/电压变化很慢, 2Hz 足够看, 不用 5Hz 跟遥测抢带宽
#define BLE_BATT_PERIOD_MS 500
// 单体电压变化超过这个数 (mV) 才推 —— 电压一直在小幅抖动, 不设阈值的话每秒都在推 2 个包
#define BLE_CELL_DELTA_MV  3
// 485 日志包的发送间隔 (ms). 一个连接事件 (30~40ms) 发一个包, 免得把链路塞爆丢包
#define BLE_LOG_PKT_GAP_MS  35
// 拉取方多久不来要下一包就算拉完了 (ms) —— 到点解冻日志, 恢复记录
#define BLE_LOG_IDLE_MS     20000
// 最后一块发出去之后, 再留多久给手机补发缺块 (ms), 然后自动解冻日志.
// 不能无限期冻着: 手机那边的"拉完了"命令不是唯一保障 (万一没送到, 日志就一直是旧的)
#define BLE_LOG_SETTLE_MS   3000
// 日志包头: [0]'L' [1]flags [2..5]偏移 [6..9]总长
#define BLE_LOG_HDR         10
// 'L' 命令的参数: 这个值表示"拉完了, 解冻"
#define BLE_LOG_ARG_RELEASE 0xFFFF

class BleTelemetry;

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
    static const uint8_t CMD_LOG       = 'L';   // 取 485 总线日志 (参数 = 起始字节偏移)
    // 回执状态码
    static const uint8_t REPLY_DONE      = 1;
    static const uint8_t REPLY_NOSUPPORT = 0xFF;
};


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
        server_ = server;   // 留着: 日志传输要用它查对端协商出来的 MTU

        // 服务要开够 handle: 每个特征占 3 个 (特征声明 + 值 + CCCD 描述符),
        // 服务本身还要占 1 个. 库的默认值是 15 —— 4 个特征时刚好够用 (12+1),
        // 加到第 5 个 (日志特征) 就不够了: esp_ble_gatts_add_char 失败, 特征建不出来.
        // 症状特别坑: 前 4 个特征一切正常 (车速/电池都好), 只有第 5 个静默失灵 ——
        // 手机订阅不到它, 板子往一个 handle 都没拿到的特征上发通知也发不出去,
        // 表现就是"点保存日志永远在导出中". 这里给 25 个, 以后再加特征也够.
        BLEService* svc = server->createService(BLEUUID(std::string(BLE_SERVICE_UUID)), 25);

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

        logChar_ = svc->createCharacteristic(BLE_LOG_UUID,
                                            BLECharacteristic::PROPERTY_NOTIFY |
                                            BLECharacteristic::PROPERTY_READ);
        logChar_->addDescriptor(new BLE2902());

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
        snap_.frameLen = 0;
        snap_.frameSeq = 0;

        // 快照互斥量 —— 主循环写, BLE 任务读
        if (!snapLock_) snapLock_ = xSemaphoreCreateMutex();
        if (snapLock_) {
            if (xTaskCreate(&BleTelemetry::taskTrampoline, "ble", BLE_TASK_STACK, this,
                            BLE_TASK_PRIO, nullptr) == pdPASS) {
                taskRunning_ = true;
            } else {
                // 建不起来就退回主循环发通知 —— 慢一点, 但至少还能看车速.
                // (症状要是变成"什么都不显示", 排查方向就完全不一样了)
                Serial.println("[BLE] BLE 任务建不起来, 退回主循环发通知");
            }
        }

        Serial.printf("[BLE] 已启动 %s, 剩余堆 %u 字节\n",
                      BLE_DEVICE_NAME, (unsigned)ESP.getFreeHeap());
        return true;
    }

    bool connected() const { return connected_; }

    // 存一份最新状态, 由 BLE 任务按 BLE_NOTIFY_PERIOD_MS 推给手机
    void publish(const BleTelemetryData& d) {
        lock();
        snap_.data = d;
        unlock();
    }

    // 电池信息 —— BMS 应答解析出来的结果
    void publishBattery(const BmsInfo& info) {
        lock();
        snap_.bms = info;
        unlock();
    }

    // BMS 解析诊断计数 —— 回答"为什么没数据"
    void publishDiag(const BmsDiag& d) {
        lock();
        snap_.diag = d;
        unlock();
    }

    // 485 总线上的原始帧 —— 让小程序能看到 ECU 到底在问什么
    // 注意: 只是"最新一帧"的快照, 总线一秒钟有几十帧, 这里只留最后一帧.
    // 完整日志要用 'L' 命令从 bus_log.h 里拉.
    void pushFrame(const uint8_t* raw, size_t len) {
        if (!raw || len == 0) return;
        if (len > sizeof(snap_.frame)) len = sizeof(snap_.frame);
        lock();
        memcpy(snap_.frame, raw, len);
        snap_.frameLen = (uint16_t)len;
        snap_.frameSeq++;
        unlock();
    }

    // ---- WiFi 热点遥控 (蓝牙优先方案的核心) ----

    // 命令回调只记下命令, 主循环 handle() 里执行, 回统一走 BLE 任务发
    void queueCommand(uint8_t cmd, uint16_t arg, bool hasArg, uint16_t mtu = 0) {
        lock();
        cmdByte_ = cmd;
        cmdArg_ = arg;
        cmdHasArg_ = hasArg;
        cmdMtu_ = mtu;
        cmdPending_ = true;
        unlock();
    }

    // 命令执行的钩子 —— 主程序 (.ino) 注入真正的 WiFi 开关实现, 该头不依赖 ota_server.h
    typedef void (*ApToggleFn)(bool on);
    void setApToggle(ApToggleFn fn) { apToggle_ = fn; }

    // 版本号的提供者 —— 主程序 (.ino) 注入 (ota_config.h 里的 OTA_FW_VERSION)
    typedef void (*VersionFn)(char* out, size_t cap);
    void setVersionProvider(VersionFn fn) { versionFn_ = fn; }

    bool cmdPending() const { return cmdPending_; }

    // 主循环里调, 不阻塞. 这里只做两件事:
    //   1) 执行排队里的命令 (WiFi 开关必须在这里做, 不能在蓝牙任务里动 WiFi)
    //   2) 断连后重新广播
    // 通知一律不在这里发 —— 见文件头那段说明
    void handle() {
        unsigned long now = millis();

        if (cmdPending_) {
            uint8_t cmd; uint16_t arg; bool hasArg; uint16_t mtu;
            lock();
            cmd = cmdByte_; arg = cmdArg_; hasArg = cmdHasArg_; mtu = cmdMtu_;
            cmdPending_ = false;
            unlock();

            if (cmd == BleCommand::CMD_VERSION) {
                if (versionFn_) {
                    char ver[20] = {0};
                    versionFn_(ver, sizeof(ver));
                    lock();
                    strncpy(snap_.version, ver, sizeof(snap_.version) - 1);
                    snap_.version[sizeof(snap_.version) - 1] = '\0';
                    snap_.haveVersion = true;
                    unlock();
                } else {
                    lock();
                    snap_.replyCmd = cmd;
                    snap_.replyState = BleCommand::REPLY_NOSUPPORT;
                    snap_.haveReply = true;
                    unlock();
                }
            } else if (cmd == BleCommand::CMD_LOG) {
                // 取 485 日志: 参数是"从第几个字节开始拉", 0xFFFF = 拉完了, 解冻.
                // mtu 是手机那边协商出来的 MTU —— 手机知道得更准 (iOS 上有时
                // 板子这边看到的还是默认值), 拿它决定一个包带多少字节.
                lock();
                logReqPending_ = true;
                logReqOffset_ = arg;
                logReqRelease_ = (hasArg && arg == BLE_LOG_ARG_RELEASE);
                logMtu_ = mtu;
                unlock();
            } else if (apToggle_) {
                apToggle_(cmd == BleCommand::CMD_START_AP);
                lock();
                snap_.replyCmd = cmd;
                snap_.replyState = BleCommand::REPLY_DONE;
                snap_.haveReply = true;
                unlock();
            } else {
                lock();
                snap_.replyCmd = cmd;
                snap_.replyState = BleCommand::REPLY_NOSUPPORT;
                snap_.haveReply = true;
                unlock();
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

        // BLE 任务建不起来时的兜底: 通知在主循环里发 (慢, 但有)
        if (!taskRunning_) serviceNotifications();
    }

private:
    // ================= BLE 任务 =================

    static void taskTrampoline(void* arg) {
        static_cast<BleTelemetry*>(arg)->bleTask();
    }

    void bleTask() {
        for (;;) {
            bool busy = serviceNotifications();
            vTaskDelay(pdMS_TO_TICKS(busy ? BLE_TASK_TICK_MS : 20));
        }
    }

    // 发一轮通知. 返回 true = "这一轮有活要干" (日志在传, 或者刚发过东西).
    //
    // 正常情况下只有 BLE 任务调它. 但如果 BLE 任务建不起来 (堆不够),
    // 就退回主循环调 —— 慢一点, 但总比"什么都不显示"好查.
    bool serviceNotifications() {
        // 1) 取快照 (主循环刚写进来的)
        Snapshot snap;
        lock();
        snap = snap_;
        unlock();

        // 2) 命令回执 / 版本号
        if (snap.haveReply) {
            lock();
            snap_.haveReply = false;
            unlock();
            uint8_t b[2] = { snap.replyCmd, snap.replyState };
            notifyCmd(b, sizeof(b));
        }
        if (snap.haveVersion) {
            lock();
            snap_.haveVersion = false;
            char ver[20];
            memcpy(ver, snap_.version, sizeof(ver));
            unlock();
            size_t n = strlen(ver);
            if (n > 18) n = 18;                    // 20 字节通知里留 2 字节头
            uint8_t b[20];
            b[0] = BleCommand::CMD_VERSION;
            memcpy(b + 1, ver, n);
            notifyCmd(b, n + 1);
        }

        // 3) 日志拉取 (用户主动点的).
        //
        // ⚠️ **绝不能因为"日志还在传"就 return 掉** ——
        //    2026-11-06 踩过: 之前这里是 `if (pumpLog()) return true;`,
        //    拉完日志之后 BLE 任务永远卡在"传输中", 遥测/电池再也不发了.
        //    用户看到的症状是"速度固定在 19.6km/h 不动 + 电池没数据" ——
        //    而固件其实活着 (485 应答一切正常, 抓的日志里能看到), 只是通知没发出去.
        //    手机的"拉完了"命令只是加速手段, 不能是唯一保障.
        bool logBusy = pumpLog();

        if (!connected_) return false;

        unsigned long now = millis();

        // 4) 遥测 (车速等) —— 每秒 5 包
        if (now - lastNotify_ >= BLE_NOTIFY_PERIOD_MS) {
            lastNotify_ = now;
            uint8_t buf[20];
            buf[0]  = snap.data.speed_dm & 0xFF;
            buf[1]  = snap.data.speed_dm >> 8;
            buf[2]  = snap.data.rpm & 0xFF;
            buf[3]  = snap.data.rpm >> 8;
            buf[4]  = snap.data.voltage & 0xFF;
            buf[5]  = snap.data.voltage >> 8;
            buf[6]  = snap.data.current & 0xFF;
            buf[7]  = (uint16_t)(snap.data.current >> 8) & 0xFF;
            buf[8]  = snap.data.protocol_speed;
            buf[9]  = snap.data.error;
            buf[10] = snap.data.unlocked;
            buf[11] = snap.data.glitch_count;
            buf[12] = snap.data.q_count & 0xFF;
            buf[13] = snap.data.q_count >> 8;
            buf[14] = snap.data.a_count & 0xFF;
            buf[15] = snap.data.a_count >> 8;
            buf[16] = snap.data.pulse_count & 0xFF;
            buf[17] = snap.data.pulse_count >> 8;
            buf[18] = snap.data.bad_cs;
            buf[19] = snap.data.interval_x10;
            telemetry_->setValue(buf, sizeof(buf));
            telemetry_->notify();
        }

        // 5) 电池 (状态每秒两次; 单体电压/型号串变了才推)
        if (now - lastBattery_ >= BLE_BATT_PERIOD_MS) {
            lastBattery_ = now;
            sendBattery(snap.bms, snap.diag);
        }

        // 6) 485 帧 (最新一帧, 分块推, 带块序号, 丢块手机能看出来).
        //    拉日志期间不发 —— 日志本身就是完整的 485 数据, 别抢带宽.
        if (!logBusy && snap.frameSeq != lastFrameSeq_) {
            lastFrameSeq_ = snap.frameSeq;
            sendFrameChunks(snap.frame, snap.frameLen);
        }

        return true;
    }

    void notifyCmd(const uint8_t* data, size_t len) {
        if (!cmdChar_ || !connected_) return;
        cmdChar_->setValue((uint8_t*)data, len);
        cmdChar_->notify();
    }

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

    void sendBattery(const BmsInfo& bms, const BmsDiag& diag) {
        if (bms.hasStatus) {
            uint8_t p[BLE_BMSG_STATUS_LEN];
            size_t n = bleBatteryPackStatus(&bms, p);
            sendBatteryMsg(BLE_BMSG_STATUS, p, n);
        }

        // 单体电压: 变了才推 (13 串 = 26 字节, 要分 2 片).
        // 阈值: 差 1~2mV 那种抖动不算"变了" —— 不设阈值的话每秒都在推 2 个包,
        // 跟遥测抢带宽 (以前蓝牙通知突发把 485 应答拖过 20ms, 就是这么来的).
        if (bms.hasCells && cellsChanged(bms)) {
            uint8_t p[BMS_MAX_CELLS * 2];
            size_t n = bleBatteryPackCells(&bms, p);
            sendBatteryMsg(BLE_BMSG_CELLS, p, n);
        }

        // 型号串: 变了才推
        if (bms.hasModel) {
            uint16_t h = hashBytes((const uint8_t*)bms.model, strlen(bms.model));
            if (h != lastModelHash_) {
                lastModelHash_ = h;
                uint8_t p[BMS_MODEL_CHARS];
                size_t n = bleBatteryPackModel(&bms, p);
                sendBatteryMsg(BLE_BMSG_MODEL, p, n);
            }
        }

        // 诊断: 变了才推 (计数变了才有新信息), 而且最多一秒一次 —— 计数每秒都在变,
        // 不限速的话每秒多推好几个包, 白白跟遥测抢带宽
        uint16_t dh = hashBytes((const uint8_t*)&diag, sizeof(diag));
        if (dh != lastDiagHash_ && millis() - lastDiagAt_ >= 1000) {
            lastDiagHash_ = dh;
            lastDiagAt_ = millis();
            uint8_t p[BLE_BMSG_DIAG_LEN];
            size_t n = bleBatteryPackDiag(&diag, p);
            sendBatteryMsg(BLE_BMSG_DIAG, p, n);
        }
    }

    // 单体电压变化够大没有 (超过 BLE_CELL_DELTA_MV mV 才算变了)
    bool cellsChanged(const BmsInfo& bms) {
        if (!lastCellsValid_ || bms.cellCount != lastCellCount_) {
            lastCellsValid_ = true;
            lastCellCount_ = bms.cellCount;
            memcpy(lastCells_, bms.cellMv, bms.cellCount * 2);
            return true;
        }
        for (uint8_t i = 0; i < bms.cellCount; i++) {
            uint16_t d = (bms.cellMv[i] > lastCells_[i]) ? (bms.cellMv[i] - lastCells_[i])
                                                         : (lastCells_[i] - bms.cellMv[i]);
            if (d >= BLE_CELL_DELTA_MV) {
                memcpy(lastCells_, bms.cellMv, bms.cellCount * 2);
                return true;
            }
        }
        return false;
    }

    // 485 帧分块推. 每块带 1 字节块序号 —— 手机靠它把块拼回去, 丢块也能看出来.
    void sendFrameChunks(const uint8_t* frame, uint16_t len) {
        if (!frame || len == 0) return;
        const size_t CHUNK = 16;      // 16 + 1 字节序号 = 17, 任何 MTU 都塞得下
        uint8_t chunk[17];
        uint8_t idx = 0;
        size_t off = 0;
        while (off < len) {
            size_t n = len - off;
            if (n > CHUNK) n = CHUNK;
            chunk[0] = idx++;
            memcpy(chunk + 1, frame + off, n);
            frame_->setValue(chunk, n + 1);
            frame_->notify();
            off += n;
        }
    }

    // ================= 485 日志拉取 =================

    // 手机端对端协商出来的 MTU (拿不到就按最小的 23 算)
    uint16_t peerMtu() {
        if (!server_) return 23;
        std::map<uint16_t, conn_status_t> peers = server_->getPeerDevices(false);
        for (std::map<uint16_t, conn_status_t>::iterator it = peers.begin();
             it != peers.end(); ++it) {
            return server_->getPeerMTU(it->first);
        }
        return 23;
    }

    // 一个日志包能带多少字节文本.
    //
    // MTU 取"板子看到的"和"手机报的"里**小**的那个 —— 保守一点:
    // 包超过对端真正能吃的大小时, 蓝牙协议栈会把它**截断**发出去, 手机收到的是
    // 半个包, 偏移对不上 → 手机以为丢包 → 一直要求重发 → 永远拉不完.
    size_t logTextCapacity() {
        uint16_t mtu = peerMtu();
        if (logMtu_ >= 23 && logMtu_ < mtu) mtu = logMtu_;
        size_t cap = (mtu > 3) ? (size_t)(mtu - 3) : 20;
        if (cap > 180) cap = 180;               // 别超过对端一般能吃的量
        if (cap <= BLE_LOG_HDR) cap = BLE_LOG_HDR + 8;
        return cap - BLE_LOG_HDR;
    }

    // 处理手机发来的拉取请求. 返回 true 表示"这一轮日志传输还没完"
    bool pumpLog() {
        bool req = false, release = false;
        uint16_t offset = 0;
        lock();
        if (logReqPending_) {
            logReqPending_ = false;
            req = true;
            release = logReqRelease_;
            offset = logReqOffset_;
        }
        unlock();

        if (release) {
            endLogTransfer();
            return false;
        }

        if (req) {
            logLastReqMs_ = millis();
            if (offset == 0) {
                // 新的拉取: 冻结日志, 从头开始 —— 拿的是**此刻**的内容
                busLog().freeze(true);
                logCap_ = logTextCapacity();
                logWriter_.begin(0);
                logTotalBytes_ = logWriter_.total();
                logSentBytes_ = 0;
                logFinished_ = false;
                logDoneAt_ = 0;
            } else {
                // 断点续传: 从头重新生成, 跳过前 offset 个字节.
                // 日志是冻结的, 所以重新生成的文本跟刚才一模一样, 偏移对得上.
                logWriter_.begin(offset);
                logTotalBytes_ = logWriter_.total();
                logSentBytes_ = offset;
                logFinished_ = false;      // 还没发完, 继续发
            }
            logActive_ = true;
        }

        if (!logActive_) return false;

        unsigned long now = millis();
        if (logFinished_) {
            // 最后一块已经发出去了. 给手机 3 秒补发缺块的机会, 然后自己收摊
            // (解冻日志, 恢复记录). **不能无限期冻着** —— 手机那边要是没把
            // "拉完了"送到 (那条命令不能是唯一保障), 日志就一直是旧的.
            if (now - logDoneAt_ > BLE_LOG_SETTLE_MS) endLogTransfer();
            return false;
        }
        if (now - logLastReqMs_ > BLE_LOG_IDLE_MS) {
            // 手机半天不来要下一包 → 当作拉完了, 解冻恢复记录
            endLogTransfer();
            return false;
        }
        if (now - logLastPktMs_ < BLE_LOG_PKT_GAP_MS) return false;
        if (!connected_ || !logChar_) return false;
        logLastPktMs_ = now;

        // 生成一块文本
        size_t cap = logCap_ ? logCap_ : logTextCapacity();
        bool done = false;
        size_t n = logWriter_.next(logChunk_, cap, &done);

        // 打包: [0]'L' [1]flags [2..5]偏移 [6..9]总长 [10..]文本
        uint8_t pkt[BLE_LOG_HDR + 180];
        pkt[0] = BleCommand::CMD_LOG;
        pkt[1] = done ? 0x01 : 0x00;
        uint32_t off = logSentBytes_;
        pkt[2] = (uint8_t)(off & 0xFF);
        pkt[3] = (uint8_t)((off >> 8) & 0xFF);
        pkt[4] = (uint8_t)((off >> 16) & 0xFF);
        pkt[5] = (uint8_t)((off >> 24) & 0xFF);
        uint32_t total = logTotalBytes_;
        pkt[6] = (uint8_t)(total & 0xFF);
        pkt[7] = (uint8_t)((total >> 8) & 0xFF);
        pkt[8] = (uint8_t)((total >> 16) & 0xFF);
        pkt[9] = (uint8_t)((total >> 24) & 0xFF);
        if (n) memcpy(pkt + BLE_LOG_HDR, logChunk_, n);

        logChar_->setValue(pkt, BLE_LOG_HDR + n);
        logChar_->notify();

        logSentBytes_ += (uint32_t)n;
        // 每发一块就续一次命 —— 传输慢的时候 (比如对端 MTU 很小, 一包只带十来个字节,
        // 整份日志要传几十秒) 不能因为"手机没再发请求"就把传输掐了.
        logLastReqMs_ = now;
        if (done) {
            // 最后一块. 日志继续冻着 (手机要是发现缺口还能让我们重发),
            // 3 秒后自动解冻 (或者等手机发"拉完了"那条命令).
            logFinished_ = true;
            logDoneAt_ = now;
        }
        return true;
    }

    void endLogTransfer() {
        if (!logActive_) return;
        logActive_ = false;
        busLog().freeze(false);
        logSentBytes_ = 0;
        logTotalBytes_ = 0;
        logCap_ = 0;
        logFinished_ = false;
    }

    static uint16_t hashBytes(const uint8_t* p, size_t n) {
        uint16_t h = 0x1D0F;
        for (size_t i = 0; i < n; i++) h = (uint16_t)((h * 31) ^ p[i]);
        return h;
    }

    void lock() { if (snapLock_) xSemaphoreTake(snapLock_, portMAX_DELAY); }
    void unlock() { if (snapLock_) xSemaphoreGive(snapLock_); }

    // ================= 内部状态 =================

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
        // 不把状态清掉的话, 重连后这两个消息永远不会再发, 电池面板只剩状态没有单体条
        lastCellsValid_ = false;
        lastModelHash_ = 0;
        lastDiagHash_ = 0;
        endLogTransfer();
        Serial.println("[BLE] 手机断开, 稍后重新广播");
        advRestartAt_ = millis() + 300;
    }

    // 主循环 → BLE 任务 的交接数据
    struct Snapshot {
        BleTelemetryData data;
        BmsInfo bms;
        BmsDiag  diag;
        uint8_t  frame[64];
        uint16_t frameLen;
        uint32_t frameSeq;
        bool     haveReply;
        uint8_t  replyCmd;
        uint8_t  replyState;
        bool     haveVersion;
        char     version[20];
    };

    Snapshot snap_ = {};
    SemaphoreHandle_t snapLock_ = nullptr;
    bool taskRunning_ = false;

    ServerCb serverCb_;
    BleCmdCb cmdCb_;
    BLEServer* server_ = nullptr;
    BLECharacteristic* telemetry_ = nullptr;
    BLECharacteristic* frame_ = nullptr;
    BLECharacteristic* battery_ = nullptr;
    BLECharacteristic* cmdChar_ = nullptr;
    BLECharacteristic* logChar_ = nullptr;
    uint8_t cmdByte_ = 0;
    uint16_t cmdArg_ = 0;
    bool cmdHasArg_ = false;
    uint16_t cmdMtu_ = 0;
    bool cmdPending_ = false;
    ApToggleFn apToggle_ = nullptr;
    VersionFn versionFn_ = nullptr;
    bool connected_ = false;
    unsigned long lastNotify_ = 0;
    unsigned long lastBattery_ = 0;
    unsigned long advRestartAt_ = 0;
    unsigned long lastDisconnectAt_ = 0;
    unsigned long lastAdvCheckAt_ = 0;
    uint32_t lastFrameSeq_ = 0;

    // 单体电压/型号串/诊断 的"变了才推"状态
    bool     lastCellsValid_ = false;
    uint8_t  lastCellCount_ = 0;
    uint16_t lastCells_[BMS_MAX_CELLS] = {0};
    uint16_t lastModelHash_ = 0;
    uint16_t lastDiagHash_ = 0;
    unsigned long lastDiagAt_ = 0;

    // 日志传输状态
    bool     logActive_ = false;
    bool     logFinished_ = false;
    bool     logReqPending_ = false;
    bool     logReqRelease_ = false;
    uint16_t logReqOffset_ = 0;
    uint16_t logMtu_ = 0;        // 手机报上来的 MTU (0 = 不知道)
    unsigned long logDoneAt_ = 0;  // 最后一块发出去的时刻
    unsigned long logLastReqMs_ = 0;
    unsigned long logLastPktMs_ = 0;
    uint32_t logTotalBytes_ = 0;
    uint32_t logSentBytes_ = 0;
    size_t   logCap_ = 0;
    BusLogWriter logWriter_;
    char     logChunk_[180];
};

// 命令特征写入回调 —— 只排队, 不执行 (WiFi 开关必须回主循环)
inline void BleCmdCb::onWrite(BLECharacteristic* ch, esp_ble_gatts_cb_param_t* param) {
    if (!owner_ || !param) return;
    uint16_t len = param->write.len;
    if (len == 0) return;
    uint8_t cmd;
    uint16_t arg = 0;
    uint16_t mtu = 0;
    bool hasArg = false;
    if (len >= 3) {
        // 带参数: [0]=命令 [1..2]=参数 (小端 u16).
        // 小程序拉日志用这个格式 ('L' + 偏移), 命令回执还是 1 个字节的老格式.
        cmd = param->write.value[0];
        arg = (uint16_t)param->write.value[1] | ((uint16_t)param->write.value[2] << 8);
        hasArg = true;
        if (len >= 5) {
            // 再带 2 字节: 手机协商出来的 MTU (小端 u16)
            mtu = (uint16_t)param->write.value[3] | ((uint16_t)param->write.value[4] << 8);
        }
    } else {
        // 取最后一个字节: 小程序写 1 字节就是它自己; 而 gatttool 之类的工具写 "0x57"
        // 会被当成 2 字节大端 (00 57), 取头就拿到 0x00 了.
        cmd = param->write.value[len - 1];
    }
    owner_->queueCommand(cmd, arg, hasArg, mtu);
    Serial.printf("[BLE] 命令 0x%02X 已排队 (arg=%u)\n", cmd, (unsigned)arg);
}

#endif
