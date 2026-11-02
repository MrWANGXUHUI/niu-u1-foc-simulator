/**
 * OTA 升级服务 — 微信小程序通过 WiFi 局域网直传固件
 *
 * 用法:
 *   手机连 ESP32 的 SoftAP (NIU-U1-OTA / niu12345)
 *   小程序用 wx.request POST 到 http://192.168.4.1:8080/ota/*
 *
 * HTTP 接口:
 *   GET  /ota/info    → 设备状态 JSON
 *   POST /ota/begin   → body JSON {"size":<字节数>,"md5":"<32位hex>"} 开始接收
 *   POST /ota/chunk   → body = base64 文本, 头 X-Offset = 原始字节偏移
 *   POST /ota/end     → 校验 + 切换启动分区 + 重启
 *   POST /ota/abort   → 放弃本次升级
 *
 * 几个不能违反的约束 (都是踩过的坑, 改代码前先读):
 *
 *   1) 固件必须 base64 编码后再 POST.
 *      Arduino core 的 WebServer 把 body 存成 String(plainBuf)
 *      (libraries/WebServer/src/Parsing.cpp), 按 C 字符串处理, 遇 0x00 就截断 ——
 *      原始二进制会静默损坏.
 *
 *   2) Update.begin(size, U_FLASH) 走 esp_ota_get_next_update_partition(),
 *      分区表里有 ota_0/ota_1 就会自动写另一个槽, 当前跑着的固件不受影响.
 *
 *   3) size 必须传准. Update.begin() 本身不擦除 —— 擦除是每写满一个 4KB 扇区时
 *      顺带做的 (Updater.cpp 的 _writeBuffer), 所以整包传输过程中不会长时间卡住
 *      485 响应. 但 size 超过分区大小会直接被拒 (UPDATE_ERROR_SIZE).
 *
 *   4) Update.end(false) 要求"收到的字节数 == size" 才激活新分区,
 *      截断的镜像永远不会生效. md5 校验也在 end() 内部做.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>

#include "ota_config.h"

// 传输中断多久判定为失败并放弃 (ms). 小程序每一块几毫秒就到, 15s 没有任何数据就是掉了
#define OTA_RX_TIMEOUT_MS   15000

class OtaService {
public:
    enum State { IDLE, RECEIVING, APPLIED };

    OtaService() : server(OTA_HTTP_PORT) {}

    void begin() {
#if OTA_AP_DEFAULT_OFF
        // 默认不开热点 —— WiFi+BLE 共存会把 BLE 广播限到 ~16 秒一次, iPhone 扫不到.
        // 平时纯蓝牙; 小程序通过 BLE 发 'W' 命令才临时开热点, OTA 完重启自动回到默认关.
        // 注意: server.begin() 必须等 WiFi 栈起来才能调 (lwIP 没初始化时开监听
        // 会 tcpip_send_msg_wait_sem assert 反复 panic), 所以挪到 startAP() 里.
        Serial.println("[OTA] WiFi 默认关 (蓝牙优先), 蓝牙 'W' 命令可临时开热点");
#else
        startAP();
#endif
    }

    // 开热点. BLE 命令触发, 每次都重新配置一遍 (信道/功率在 softAP 里设)
    void startAP() {
        IPAddress ip(OTA_AP_IP);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
        // 信道见 ota_config.h 的说明
        bool ok = WiFi.softAP(OTA_AP_SSID, OTA_AP_PASSWORD, OTA_AP_CHANNEL);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        apOn_ = true;
        // 每次都重绑: WiFi.mode(WIFI_OFF) 会把 lwIP 一起销毁, 老的监听 socket
        // 就废了, 不重绑的话"关热点再开"之后 HTTP 端口收不到任何请求.
        // WebServer::begin() 内部先 close() 再监听, 所以重复调是安全的.
        bindServer();
        Serial.printf("[OTA] 热点 %s %s  信道%d  IP %s:%d\n",
                      OTA_AP_SSID, ok ? "已启动" : "启动失败", OTA_AP_CHANNEL,
                      ip.toString().c_str(), OTA_HTTP_PORT);
    }

    // 关热点, 把射频还给 BLE.
    // 注意: 不能用 WiFi.mode(WIFI_OFF) —— 那会把整个 WiFi 协议栈拆掉 (esp_wifi_stop
    // + lwIP 反初始化), 之后想再开热点要重新初始化一堆东西, 慢且容易出问题.
    // 这里只把模式切到 WIFI_MODE_NULL: 热点停止广播, 但协议栈留着, startAP() 随时能开回来.
    void stopAP() {
        WiFi.softAPdisconnect(true);
        esp_wifi_set_mode(WIFI_MODE_NULL);
        apOn_ = false;
        Serial.println("[OTA] 热点已关, BLE 独占射频");
    }

    bool apOn() const { return apOn_; }

    // 路由注册 + 监听. 只能等 WiFi 栈起来 (第一次 startAP) 才能做:
    // WiFi 从没初始化过就 server.begin() 会 lwIP assert 反复 panic
    // (tcpip_send_msg_wait_sem: Invalid mbox).
    void bindServer() {
        bindRoutes();
        server.begin();
        serverBound_ = true;
    }

    // 路由注册只需要做一次 (路由表不变), 监听每次开热点重做
    void bindRoutes() {
        if (routesBound_) return;
        server.on("/ota/info",  HTTP_GET,  [this] { handleInfo();  });
        server.on("/ota/begin", HTTP_POST, [this] { handleBegin(); });
        server.on("/ota/chunk", HTTP_POST, [this] { handleChunk(); });
        server.on("/ota/end",   HTTP_POST, [this] { handleEnd();   });
        server.on("/ota/abort", HTTP_POST, [this] { handleAbort(); });
        server.onNotFound([this] {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        });
        routesBound_ = true;
    }

    // 主循环里调, 不阻塞
    void handle() {
        if (apOn_) server.handleClient();

        // 传输卡住太久 → 放弃, 否则下一次 Update.begin() 会因为"已有升级在进行"直接失败
        if (state_ == RECEIVING && millis() - lastRxAt_ > OTA_RX_TIMEOUT_MS) {
            Serial.printf("[OTA] 传输超时, 放弃 (已收 %u 字节)\n", written_);
            Update.abort();
            state_ = IDLE;
        }

        // /ota/end 之后留一点时间让 HTTP 响应发出去再重启
        if (restartPending_ && millis() >= restartAt_) {
            ESP.restart();
        }
    }

    bool isBusy() const { return state_ == RECEIVING; }
    uint32_t progress() const { return written_; }
    uint32_t total() const { return totalSize_; }
    const char* version() const { return OTA_FW_VERSION; }
    const char* stateName() const {
        switch (state_) {
            case RECEIVING: return "receiving";
            case APPLIED:   return "applied";
            default:        return "idle";
        }
    }

private:
    WebServer server;
    bool apOn_ = false;
    bool serverBound_ = false;
    bool routesBound_ = false;
    State state_ = IDLE;
    uint32_t totalSize_ = 0;
    uint32_t written_ = 0;
    uint32_t lastRxAt_ = 0;
    int lastLogPct_ = 0;
    char md5_[33] = {0};
    bool restartPending_ = false;
    uint32_t restartAt_ = 0;

    // ---------- base64 解码 ----------
    // core 自带的 base64 类只有 encode 没有 decode, 自己写一个.
    static int b64Val(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    // 返回解码后字节数, 失败返回 -1. 忽略 '=' padding 和换行
    static int b64Decode(const char* in, size_t inLen, uint8_t* out, size_t outCap) {
        uint32_t acc = 0;
        int bits = 0;
        size_t n = 0;
        for (size_t i = 0; i < inLen; i++) {
            char c = in[i];
            if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
            int v = b64Val(c);
            if (v < 0) return -1;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n >= outCap) return -1;
                out[n++] = (uint8_t)((acc >> bits) & 0xFF);
                acc &= (bits == 0) ? 0 : ((1u << bits) - 1);   // 只留还没用完的位
            }
        }
        return (int)n;
    }

    // ---------- 极简 JSON 取值 (只认 "key": 值 这种扁平格式) ----------
    static uint32_t jsonGetUInt(const String& body, const char* key) {
        String pat = String("\"") + key + "\"";
        int i = body.indexOf(pat);
        if (i < 0) return 0;
        i = body.indexOf(':', i + pat.length());
        if (i < 0) return 0;
        i++;
        while (i < body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
        uint32_t v = 0;
        bool any = false;
        while (i < body.length() && body[i] >= '0' && body[i] <= '9') {
            v = v * 10 + (uint32_t)(body[i] - '0');
            i++;
            any = true;
        }
        return any ? v : 0;
    }

    static bool jsonGetStr(const String& body, const char* key, char* out, size_t cap) {
        String pat = String("\"") + key + "\"";
        int i = body.indexOf(pat);
        if (i < 0) return false;
        i = body.indexOf(':', i + pat.length());
        if (i < 0) return false;
        i = body.indexOf('"', i);
        if (i < 0) return false;
        i++;
        int end = body.indexOf('"', i);
        if (end < 0) return false;
        size_t len = (size_t)(end - i);
        if (len == 0 || len >= cap) return false;
        memcpy(out, body.c_str() + i, len);
        out[len] = '\0';
        return true;
    }

    // ---------- 路由处理 ----------
    String infoJson() {
        const esp_partition_t* running = esp_ota_get_running_partition();
        // 不用 ESP.getSketchSize(): 它内部会 esp_image_verify(), 把 1.5MB 的
        // app 镜像 SHA256 重算一遍 (走硬件 SHA, 而 C3 的加密 GDMA 通道跟 WiFi
        // 驱动共用). 一次 info 请求几十毫秒, 还可能跟热点上的加密抢通道.
        // 分区大小直接查分区表就够.
        uint32_t slotSize = running ? running->size : 0;
        char buf[512];
        // ⚠️ 参数顺序必须跟格式串里的占位符**一一对应**, 一个都不能错位 ——
        // 错位了编译器不会报错, 但运行时会拿 int 当 char* 去解引用:
        //   %s 吃到一个整数 1 → 读地址 0x1 → Load access fault → 板子重启
        // (2026-11-02 就是这么查了半天: /ota/info 一发就重启, 栈底那个
        //  "sha256/GDMA" 是 FreeRTOS 复用任务栈留下的旧数据, 跟真正的错位无关)
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"version\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"port\":%d,"
                 "\"state\":\"%s\",\"progress\":%u,\"total\":%u,\"freeHeap\":%u,\"slotSize\":%u,"
                 "\"slot\":\"%s\",\"md5\":\"%s\",\"uptime\":%lu,\"apOn\":%d}",
                 OTA_FW_VERSION,                        // %s  version
                 OTA_AP_SSID,                           // %s  ssid
                 WiFi.softAPIP().toString().c_str(),    // %s  ip
                 OTA_HTTP_PORT,                         // %d  port
                 stateName(),                           // %s  state
                 (unsigned)written_,                    // %u  progress
                 (unsigned)totalSize_,                  // %u  total
                 (unsigned)ESP.getFreeHeap(),           // %u  freeHeap
                 (unsigned)slotSize,                    // %u  slotSize
                 running ? running->label : "?",        // %s  slot
                 md5_,                                  // %s  md5
                 (unsigned long)(millis() / 1000),      // %lu uptime
                 (int)apOn_);                           // %d  apOn
        return String(buf);
    }

    void handleInfo() {
        server.send(200, "application/json", infoJson());
    }

    void handleBegin() {
        if (state_ == RECEIVING) Update.abort();

        String body = server.arg("plain");
        uint32_t size = jsonGetUInt(body, "size");
        if (size == 0 || size > OTA_MAX_FIRMWARE_SIZE) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad size\"}");
            Serial.printf("[OTA] 拒绝: size=%u 非法\n", size);
            return;
        }

        if (!Update.begin(size, U_FLASH)) {
            server.send(500, "application/json",
                        String("{\"ok\":false,\"error\":\"") + Update.errorString() + "\"}");
            Serial.printf("[OTA] Update.begin 失败: %s\n", Update.errorString());
            return;
        }

        md5_[0] = '\0';
        if (jsonGetStr(body, "md5", md5_, sizeof(md5_))) {
            if (!Update.setMD5(md5_)) {
                Update.abort();
                server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad md5\"}");
                return;
            }
        }

        totalSize_ = size;
        written_ = 0;
        lastLogPct_ = 0;
        state_ = RECEIVING;
        lastRxAt_ = millis();
        server.send(200, "application/json", "{\"ok\":true}");
        Serial.printf("[OTA] 开始接收固件: %u 字节%s\n", size, md5_[0] ? " (带 md5 校验)" : "");
    }

    void handleChunk() {
        if (state_ != RECEIVING) {
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"not receiving\"}");
            return;
        }
        String body = server.arg("plain");
        if (!body.length()) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
            return;
        }

        // 偏移必须对得上, 否则说明中间丢了一块, 后面全是错的
        uint32_t offset = written_;
        if (server.hasHeader("X-Offset")) {
            offset = (uint32_t)strtoul(server.header("X-Offset").c_str(), nullptr, 10);
        }
        if (offset != written_) {
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"offset mismatch\"}");
            Serial.printf("[OTA] 偏移不符: 头 %u, 实际 %u\n", offset, written_);
            return;
        }

        static uint8_t buf[OTA_CHUNK_SIZE];
        int n = b64Decode(body.c_str(), body.length(), buf, sizeof(buf));
        if (n < 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad base64\"}");
            return;
        }

        size_t w = Update.write(buf, (size_t)n);
        if (w != (size_t)n) {
            Update.abort();
            state_ = IDLE;
            server.send(500, "application/json",
                        String("{\"ok\":false,\"error\":\"") + Update.errorString() + "\"}");
            Serial.printf("[OTA] 写 flash 失败: %s\n", Update.errorString());
            return;
        }

        written_ += w;
        lastRxAt_ = millis();

        char buf2[96];
        snprintf(buf2, sizeof(buf2), "{\"ok\":true,\"written\":%u,\"total\":%u}", written_, totalSize_);
        server.send(200, "application/json", buf2);

        int pct = totalSize_ ? (int)((uint32_t)written_ * 100 / totalSize_) : 0;
        if (pct >= lastLogPct_ + 25 || written_ == totalSize_) {
            lastLogPct_ = pct - (pct % 25);
            Serial.printf("[OTA] 进度 %u/%u 字节 (%d%%)\n", written_, totalSize_, pct);
        }
    }

    void handleEnd() {
        if (state_ != RECEIVING) {
            server.send(409, "application/json", "{\"ok\":false,\"error\":\"not receiving\"}");
            return;
        }
        if (written_ != totalSize_) {
            Update.abort();
            state_ = IDLE;
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"size mismatch\"}");
            Serial.printf("[OTA] 长度不符: %u/%u, 放弃\n", written_, totalSize_);
            return;
        }

        // evenIfRemaining=false: 少一个字节都不激活新分区
        if (!Update.end(false)) {
            String err = Update.errorString();
            Update.abort();
            state_ = IDLE;
            server.send(500, "application/json", String("{\"ok\":false,\"error\":\"") + err + "\"}");
            Serial.printf("[OTA] 校验/激活失败: %s (保持旧固件)\n", err.c_str());
            return;
        }

        state_ = APPLIED;
        Serial.printf("[OTA] 固件已写入并激活, 准备重启\n");
        server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
        restartPending_ = true;
        restartAt_ = millis() + 800;   // 给响应留出发送时间
    }

    void handleAbort() {
        if (state_ == RECEIVING) {
            Update.abort();
            Serial.println("[OTA] 已放弃本次升级");
        }
        state_ = IDLE;
        written_ = 0;
        totalSize_ = 0;
        server.send(200, "application/json", "{\"ok\":true}");
    }
};
