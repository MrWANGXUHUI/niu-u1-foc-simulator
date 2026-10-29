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

#include "ota_config.h"

// 传输中断多久判定为失败并放弃 (ms). 小程序每一块几毫秒就到, 15s 没有任何数据就是掉了
#define OTA_RX_TIMEOUT_MS   15000

class OtaService {
public:
    enum State { IDLE, RECEIVING, APPLIED };

    OtaService() : server(OTA_HTTP_PORT) {}

    void begin() {
        IPAddress ip(OTA_AP_IP);
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
        // 信道见 ota_config.h 的说明 —— 默认的 1 信道跟 BLE 广播信道撞车, 手机连不上
        bool ok = WiFi.softAP(OTA_AP_SSID, OTA_AP_PASSWORD, OTA_AP_CHANNEL);
        WiFi.setSleep(false);   // 关掉 modem sleep, 否则 AP 会时不时把客户端踢掉
        // 发射功率从默认 19.5dBm 降到 8.5dBm.
        // 手机就贴在车边, 功率再高也没用; 而 WiFi 满功率发射的瞬时电流能到 300mA+,
        // 车上供电是 48V 降压下来的, 功率越小越不容易把板子拉棕断 (棕断=反复重启=
        // 热点掉线+屏幕闪烁). 功率不够的话手机连不上, 但距离只有一两米, 8.5dBm 绰绰有余.
        WiFi.setTxPower(WIFI_POWER_8_5dBm);

        server.on("/ota/info",  HTTP_GET,  [this] { handleInfo();  });
        server.on("/ota/begin", HTTP_POST, [this] { handleBegin(); });
        server.on("/ota/chunk", HTTP_POST, [this] { handleChunk(); });
        server.on("/ota/end",   HTTP_POST, [this] { handleEnd();   });
        server.on("/ota/abort", HTTP_POST, [this] { handleAbort(); });
        server.onNotFound([this] {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        });
        server.begin();

        Serial.printf("[OTA] 热点 %s %s  信道%d  IP %s:%d\n",
                      OTA_AP_SSID, ok ? "已启动" : "启动失败", OTA_AP_CHANNEL,
                      ip.toString().c_str(), OTA_HTTP_PORT);
    }

    // 主循环里调, 不阻塞
    void handle() {
        server.handleClient();

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
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"version\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"port\":%d,"
                 "\"state\":\"%s\",\"progress\":%u,\"total\":%u,\"freeHeap\":%u,\"flashUsed\":%u,"
                 "\"slot\":\"%s\",\"md5\":\"%s\",\"uptime\":%lu}",
                 OTA_FW_VERSION, OTA_AP_SSID, WiFi.softAPIP().toString().c_str(), OTA_HTTP_PORT,
                 stateName(), written_, totalSize_,
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getSketchSize(),
                 running ? running->label : "?", md5_,
                 (unsigned long)(millis() / 1000));
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
