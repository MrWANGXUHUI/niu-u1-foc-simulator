// 设备侧自测: 485 日志的环形缓冲 + 文本格式 + BLE 包格式
//
//   g++ -I../niu_u1_foc_simulator/src -o /tmp/test_bus_log tools/test_bus_log.cpp && /tmp/test_bus_log
//
// 为什么要有这个:
//   "日志 → 蓝牙包 → 小程序还原" 这条链上, 最容易出的错是**两边格式对不上**
//   (偏移算错一个字节, 整份日志就废了), 而这段逻辑不连板子也能测.
//   配套的小程序侧测试在 tools/test_miniprogram_ble.js.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "bus_log.h"
#include "bms_protocol.h"

#define BLE_LOG_HDR 10

static int g_fail = 0;

static void check(const char* name, bool ok, const std::string& detail = "") {
    printf("%s %s%s\n", ok ? "✓" : "✗", name, ok ? "" : ("  " + detail).c_str());
    if (!ok) g_fail++;
}

// 组一帧: 68 [a1] [a2] 68 [func] [len] [data+0x33...] [cs] 16
static void buildFrame(std::vector<uint8_t>& out, uint8_t a1, uint8_t a2,
                       uint8_t func, const std::vector<uint8_t>& plain) {
    out.clear();
    out.push_back(0x68);
    out.push_back(a1);
    out.push_back(a2);
    out.push_back(0x68);
    out.push_back(func);
    out.push_back((uint8_t)plain.size());
    for (size_t i = 0; i < plain.size(); i++) out.push_back(plain[i] + 0x33);
    uint8_t cs = 0;
    for (size_t i = 0; i < out.size(); i++) cs = (uint8_t)(cs + out[i]);
    out.push_back(cs);
    out.push_back(0x16);
}

int main(int argc, char** argv) {
    const char* pktPath = (argc > 1) ? argv[1] : "/tmp/niu485_test/packets.bin";
    const char* txtPath = (argc > 2) ? argv[2] : "/tmp/niu485_test/expect.txt";

    busLog().begin();

    // ---- 1) BMS 应答解析 (顺带验一下 bms_protocol.h) ----
    std::vector<uint8_t> f;
    buildFrame(f, 0x31, 0xCE, 0x82, {0x02, 0x19, 0x00, 0x00, 0x00, 0x00, 0x50,
                                     0x00, 0x00, 0x00, 0x16, 0x15, 0x16, 0x15, 0x14});
    BmsInfo info = {0};
    int st = bmsParseFrame(f.data(), f.size(), &info);
    check("BMS 状态帧解析成功", st == BMS_PARSE_OK);
    check("BMS 总电压 53.7V", info.voltage_dv == 537);
    check("BMS 电量 80%", info.soc == 80);
    check("BMS 温度 22/21/22/21/20",
          info.temps[0] == 22 && info.temps[1] == 21 && info.temps[2] == 22 &&
          info.temps[3] == 21 && info.temps[4] == 20);

    // ---- 55 字节的帧: 状态(15) + 单体电压(40) 拼在一起 ----
    // 2026-11-06 真车日志里抓的原始数据 (这台车的 BMS 就是这么回的):
    // V35 问的是 `2D 37` (文档记的那台车问的是 `2D 0F`), 回 55 字节.
    // 我们原来只认 15/40/43 字节 → 整帧被丢 → 小程序上电量一直是空的.
    {
        std::vector<uint8_t> plain55 = {
            0x01, 0xCD, 0x00, 0x00, 0x00, 0x66, 0x3F, 0x00, 0x01, 0x07,
            0x04, 0x04, 0x04, 0x04, 0x06,
            0x0D, 0x8F, 0x0D, 0x97, 0x0D, 0x9F, 0x0D, 0x9C, 0x0D, 0x94, 0x0D,
            0x9E, 0x0D, 0xA4, 0x0D, 0xBC, 0x0D, 0xC3, 0x0D, 0xD9, 0x0D, 0x9E,
            0x0D, 0x9F, 0x0D, 0xB6,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        buildFrame(f, 0x31, 0xCE, 0x82, plain55);
        check("55 字节帧: 整帧长度对", f.size() == 55 + 8);
        BmsInfo info3 = {0};
        int st3 = bmsParseFrame(f.data(), f.size(), &info3);
        check("55 字节帧: 解析成功", st3 == BMS_PARSE_OK);
        check("55 字节帧: 有状态", info3.hasStatus);
        check("55 字节帧: 有单体电压", info3.hasCells);
        check("55 字节帧: 总电压 46.1V", info3.voltage_dv == 461);
        check("55 字节帧: 电量 63%", info3.soc == 63);
        bool tempsOk = info3.temps[0] == 4 && info3.temps[1] == 4 &&
                       info3.temps[2] == 4 && info3.temps[3] == 4 && info3.temps[4] == 6;
        check("55 字节帧: 温度 4/4/4/4/6", tempsOk);
        check("55 字节帧: 电流 10A (0.1A 一档)", info3.current == 10);
        check("55 字节帧: 13 串", info3.cellCount == 13);
        check("55 字节帧: 第一串 3471mV", info3.cellMv[0] == 3471);
        {
            uint32_t sum = 0;
            for (uint8_t i = 0; i < info3.cellCount; i++) sum += info3.cellMv[i];
            check("55 字节帧: 单体求和 45.44V", sum == 45442);
        }
    }

    // 校验和错的帧要能认出来 (不然分不清"没数据"和"数据被干扰了")
    std::vector<uint8_t> bad = f;
    bad[bad.size() - 2] ^= 0xFF;
    BmsInfo info2 = {0};
    check("校验和错的帧认得出来", bmsParseFrame(bad.data(), bad.size(), &info2) == BMS_PARSE_BAD_CS);

    // ---- 2) 造一段总线流量 ----
    uint32_t t = 500;
    for (int cycle = 0; cycle < 30; cycle++) {
        buildFrame(f, 0x20, 0xDF, 0x02, {0x05, 0x08});
        busLog().record(BUS_LOG_DIR_RX, t, f.data(), f.size()); t += 11;
        buildFrame(f, 0x20, 0xDF, 0x82, {0x30, 0x26, 0x3C, 0x01, 0x00, 0x19,
                                         0x00, 0x00, 0x00, 0x15, 0x00, 0x00});
        busLog().record(BUS_LOG_DIR_TX, t, f.data(), f.size()); t += 25;
        buildFrame(f, 0x31, 0xCE, 0x82, {0x02, 0x19, 0x00, 0x00, 0x00, 0x00, 0x50,
                                         0x00, 0x00, 0x00, 0x16, 0x15, 0x16, 0x15, 0x14});
        busLog().record(BUS_LOG_DIR_RX, t, f.data(), f.size()); t += 160;
    }
    check("记了 90 帧", busLog().count() == 90);

    // ---- 3) 环形缓冲写满以后覆盖最老的 ----
    uint32_t firstT = 0, newFirstT = 0;
    { uint8_t d; const uint8_t* p; uint8_t l; busLog().get(0, &firstT, &d, &p, &l); }
    for (int i = 0; i < BUS_LOG_SLOTS + 10; i++) {
        buildFrame(f, 0x40, 0xBF, 0x06, {0x01, 0x11});
        busLog().record(BUS_LOG_DIR_RX, t, f.data(), f.size());
        t += 10;
    }
    { uint8_t d; const uint8_t* p; uint8_t l; busLog().get(0, &newFirstT, &d, &p, &l); }
    check("写满以后帧数封顶", busLog().count() == BUS_LOG_SLOTS);
    check("最老的被顶掉了", newFirstT > firstT);

    // ---- 4) 文本格式: 长度要跟实际写出来的对得上 (手机进度条靠它) ----
    uint32_t computed = busLog().totalTextLen();
    uint32_t actual = 0;
    for (uint16_t i = 0; i < busLog().count(); i++) {
        uint32_t ts; uint8_t dir; const uint8_t* data; uint8_t len;
        if (!busLog().get(i, &ts, &dir, &data, &len)) break;
        char line[BUS_LOG_TEXT_MAX];
        actual += (uint32_t)BusLog::formatRecord(line, sizeof(line), ts, dir, data, len);
    }
    check("文本总长算得对 (手机进度条)", computed == actual);

    // 冻结 —— 拉日志期间设备停止记录
    busLog().freeze(true);
    uint32_t before = busLog().count();
    buildFrame(f, 0x40, 0xBF, 0x06, {0x01, 0x11});
    busLog().record(BUS_LOG_DIR_RX, t, f.data(), f.size());
    check("冻结以后不再记录", busLog().count() == before);
    busLog().freeze(false);

    // ---- 5) 生成 BLE 日志包 (小程序端要能原样还原) ----
    busLog().freeze(true);
    BusLogWriter w;
    w.begin(0);
    uint32_t total = w.total();
    FILE* fp = fopen(pktPath, "wb");
    FILE* tf = fopen(txtPath, "wb");
    uint32_t sent = 0;
    int packets = 0;
    bool done = false;
    char chunk[180];
    while (!done) {
        size_t n = w.next(chunk, 170, &done);
        uint8_t pkt[BLE_LOG_HDR + 180];
        pkt[0] = 'L';
        pkt[1] = done ? 0x01 : 0x00;
        pkt[2] = (uint8_t)(sent & 0xFF);
        pkt[3] = (uint8_t)((sent >> 8) & 0xFF);
        pkt[4] = (uint8_t)((sent >> 16) & 0xFF);
        pkt[5] = (uint8_t)((sent >> 24) & 0xFF);
        pkt[6] = (uint8_t)(total & 0xFF);
        pkt[7] = (uint8_t)((total >> 8) & 0xFF);
        pkt[8] = (uint8_t)((total >> 16) & 0xFF);
        pkt[9] = (uint8_t)((total >> 24) & 0xFF);
        if (n) memcpy(pkt + BLE_LOG_HDR, chunk, n);
        // 测试文件格式: [u32 本包长度][本包字节]
        uint32_t plen = BLE_LOG_HDR + (uint32_t)n;
        fwrite(&plen, 4, 1, fp);
        fwrite(pkt, 1, plen, fp);
        fwrite(chunk, 1, n, tf);
        sent += (uint32_t)n;
        packets++;
    }
    fclose(fp);
    fclose(tf);
    check("发出去的字节数等于总长", sent == total);
    check("分了不止一个包 (块大小对)", packets > 1);

    // 断点续传: 从中间开始, 拼出来的应该跟原文的后半段一模一样
    uint32_t half = total / 2;
    BusLogWriter w2;
    w2.begin(half);
    std::string resumed;
    done = false;
    while (!done) {
        size_t n = w2.next(chunk, 170, &done);
        resumed.append(chunk, n);
    }
    check("断点续传: 跳过前半段后剩下的对得上", resumed.length() == total - half);

    printf("\n%s\n", g_fail ? "有没过的地方" : "设备侧全部通过");
    return g_fail ? 1 : 0;
}
