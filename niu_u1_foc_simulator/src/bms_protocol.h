/**
 * 小牛 U1 电池 BMS 的 485 报文解析
 *
 * 总线上的角色: V35 中控 (0x40) 是主节点, 挨个轮询 FOC(0x20) / BMS(0x31) / 仪表(0x10).
 * BMS 应答的地址对是 31 CE, 功能码 0x82 (最高位 1 = 回复), 长度字节在第 5 位.
 *
 * V35 会问 BMS 四种东西, 靠请求里的两个字节区分 (请求数据区 -0x33 反混淆后):
 *
 *   请求 2d 0f → 15 字节  电池状态   (每次轮询都问, 3 次/秒左右)
 *   请求 09 24 → 36 字节  型号串
 *   请求 02 2b → 43 字节  型号串 + 7 字节头
 *   请求 3c 28 → 40 字节  单体电压
 *
 * 15 字节状态帧的字段布局 (来源: github.com/xzfn/scooter-repair 的 docs/niu-u-civic.md
 * 「BMS返回的主要是电量」一节, 并用真车抓包逐字段核对过):
 *
 *   [0..1]  总电压   大端 uint16, 单位 0.1V     (例 0x0219 = 537 → 53.7V)
 *   [2..4]  未知
 *   [5]     电流     单位 A                     (实测 0..38A)
 *   [6]     电量     单位 %
 *   [7..9]  未知
 *   [10..14] 5 个温度, 单位 °C
 *
 * 40 字节单体电压帧: 前面是单体电压 (大端 uint16, 单位 mV), 一直到第一个 0 为止,
 * 后面是保留的 0. 13 串的 48V 电池 = 13 × 3.7V, 加起来跟状态帧的总电压对得上.
 *
 * 型号串帧: 36 字节的那帧串在开头, 43 字节的那帧串在第 7 字节之后 (前 7 字节是 14 04 55 55 55 55 01).
 * 串是 16 字节 ASCII, 形如 "BUS9LN2S60900065".
 *
 * 这个头文件不依赖 Arduino, 可以直接在 PC 上编译跑测试 (tools 里没放测试脚本,
 * 拿真车抓包日志喂进去逐字段核对即可).
 */

#ifndef BMS_PROTOCOL_H
#define BMS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>   // 这个头文件不依赖 Arduino, 要能单独在 PC 上编译

#define BMS_ADDR            0x31
#define BMS_STATUS_LEN      15
#define BMS_CELLS_LEN       40
#define BMS_MODEL_LEN       36
#define BMS_INFO_LEN        43
// 状态 + 单体电压拼在一帧里 (55 字节).
// 2026-11-06 抓真车日志发现的: 这台车的 V35 用 `2D 37` 问 BMS, BMS 回的是
// **55 字节** —— 正好是"15 字节状态帧 + 40 字节单体电压帧"拼起来.
// (文档里记的是 `2D 0F` → 15 字节状态, 另一台车/另一个 BMS 固件版本)
// 我们原来只认 15/40/43 字节, 整帧被丢掉 → 小程序上电量一直是空的.
#define BMS_FULL_LEN        55
// 数据区的缓冲区上限. 必须 >= 上面所有已知帧长, 且要比 BMS_INFO_LEN 留余量 ——
// 现在这个坑就是: 缓冲区刚好开成 43 (照着目前观测到的最大帧), 一个字节余量都没有,
// BMS 只要回一帧数据区更长的应答就写穿栈, 板子直接 PANIC 重启.
#define BMS_MAX_DATA_LEN    64
#define BMS_MAX_CELLS       20
#define BMS_TEMP_COUNT      5
#define BMS_MODEL_CHARS     16

// BMS 状态帧的固定长度字段
#define BMS_OFF_VOLTAGE     0
#define BMS_OFF_CURRENT     5
#define BMS_OFF_SOC         6
#define BMS_OFF_TEMPS       10

// BMS 电流字节的单位: 除以这个数才是 A.
// 文档 (xzfn/scooter-repair) 记的是"直接当 A", 但 2026-11-06 抓的**这台车**的日志里
// 原始字节是 76~159 —— 按 A 算就是 46V × 159A = 7.3kW, 踏板车没那么大的功率.
// 按 0.1A 算 (7.6~15.9A, 350~730W) 才合理.
// 这台车的 BMS 跟文档那台不是一套 (它问 BMS 用的是 `2D 37`, 文档记的是 `2D 0F`),
// 电流档位不一样不奇怪. 换台车要是不对, 把这个数改成 1.
#define BMS_CURRENT_SCALE   10

struct BmsInfo {
    // 状态帧 (15 字节)
    bool     hasStatus;
    uint16_t voltage_dv;              // 总电压, 0.1V
    int16_t  current;                 // 电流, A (负数 = 回充)
    uint8_t  soc;                     // 电量 %
    int8_t   temps[BMS_TEMP_COUNT];   // 温度 °C
    uint8_t  tempCount;

    // 单体电压帧 (40 字节)
    bool     hasCells;
    uint8_t  cellCount;
    uint16_t cellMv[BMS_MAX_CELLS];   // 单体电压, mV

    // 型号串帧 (36 / 43 字节)
    bool     hasModel;
    char     model[BMS_MODEL_CHARS + 1];
};

// BMS 解析诊断计数 —— 推给小程序, 用来回答"到底为什么没有电池数据".
//
// 「没收到电池报文」是个笼统的现象, 光看它没法动手. 这几个计数能把可能性拆开:
//   rxFrames  = 0            → 总线上根本没有 BMS 应答 (BMS 没回话 / 我们的分帧把它吃了)
//   rxFrames > 0, okFrames=0 → 帧收到了但解析不出来 (校验和错 = 被干扰/被撞坏)
//   okFrames > 0             → 固件这边完全正常, 问题在蓝牙这一段 (通知没送到手机)
struct BmsDiag {
    uint16_t rxFrames;     // 总线上看到的 BMS 帧数
    uint16_t okFrames;     // 解析成功数
    uint8_t  csErr;        // 校验和错的帧数
    uint8_t  lenErr;       // 长度字段对不上的帧数
    uint8_t  unknownLen;   // 是 BMS 应答但数据区长度没见过
    uint8_t  lastLen;      // 上次看到的 BMS 帧的总长度
    uint32_t ageMs;        // 距上次看到 BMS 帧过了多少毫秒 (0xFFFFFFFF = 从来没有)
    uint16_t qCount;       // 收到 V35 状态查询的次数
    uint16_t aCount;       // 我们回出去的应答次数
};

// 帧头/帧尾/地址/功能码/长度
static inline bool bmsIsReplyFrame(const uint8_t* raw, size_t len) {
    return len >= 9
        && raw[0] == 0x68
        && raw[3] == 0x68
        && raw[1] == BMS_ADDR
        && raw[len - 1] == 0x16
        && (raw[4] & 0x80) != 0;
}

// 校验和 = 从帧头累加到 CS 前一字节, 取低 8 位 (不是 CRC)
static inline bool bmsChecksumOk(const uint8_t* raw, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len - 2; i++) sum += raw[i];
    return (sum & 0xFF) == raw[len - 2];
}

// 解析结果 —— 不只是"成没成功", 还要说清楚**为什么**没成功.
// 小程序上"没收到电池报文"是个笼统的现象, 光看它没法动手:
//   BMS_PARSE_OK        = 解析出来了 (数据没问题)
//   BMS_PARSE_NOT_BMS   = 这压根不是 BMS 应答 (地址/功能码不对)
//   BMS_PARSE_BAD_CS    = 是 BMS 应答, 但校验和不对 → 总线被干扰了, 或者帧被撞坏
//   BMS_PARSE_BAD_LEN   = 是 BMS 应答, 但长度字段和实际帧长对不上 → 帧被截断/拼错
//   BMS_PARSE_UNKNOWN   = 是 BMS 应答, 校验和也对, 但数据区长度没见过 → 协议有变化
// 把这几个计数推给手机, 一眼就能分清"总线根本没数据" / "数据被干扰了" / "我们解析不了".
#define BMS_PARSE_OK        1
#define BMS_PARSE_NOT_BMS   0
#define BMS_PARSE_BAD_CS    2
#define BMS_PARSE_BAD_LEN   3
#define BMS_PARSE_UNKNOWN   4

// 解析 15 字节状态段: [0..1]总电压(大端,0.1V) [2..4]未知 [5]电流(A)
//                     [6]电量% [7..9]未知 [10..14] 5 个温度
static inline void bmsParseStatus(const uint8_t* plain, BmsInfo* out) {
    out->hasStatus = true;
    out->voltage_dv = (uint16_t)((plain[BMS_OFF_VOLTAGE] << 8) | plain[BMS_OFF_VOLTAGE + 1]);
    // 电流: 有符号 (充电是负的), 单位按 BMS_CURRENT_SCALE 换算
    out->current  = (int16_t)((int8_t)plain[BMS_OFF_CURRENT]) / BMS_CURRENT_SCALE;
    out->soc      = plain[BMS_OFF_SOC];
    out->tempCount = BMS_TEMP_COUNT;
    for (uint8_t i = 0; i < BMS_TEMP_COUNT; i++) {
        out->temps[i] = (int8_t)plain[BMS_OFF_TEMPS + i];
    }
}

// 解析 40 字节单体电压段: 前面是大端 u16 (mV), 一直到第一个 0 为止, 后面是保留的 0
static inline void bmsParseCells(const uint8_t* plain, BmsInfo* out) {
    out->hasCells = true;
    out->cellCount = 0;
    for (uint8_t i = 0; i + 1 < BMS_CELLS_LEN && out->cellCount < BMS_MAX_CELLS; i += 2) {
        uint16_t mv = (uint16_t)((plain[i] << 8) | plain[i + 1]);
        if (mv == 0) break;              // 后面是保留的 0
        out->cellMv[out->cellCount++] = mv;
    }
}

// 解析一帧 BMS 应答. 返回 BMS_PARSE_* 之一.
// 数据区每个字节发送时 +0x33 混淆, 这里 -0x33 反混淆.
static inline int bmsParseFrame(const uint8_t* raw, size_t len, BmsInfo* out) {
    if (!out || !bmsIsReplyFrame(raw, len)) return BMS_PARSE_NOT_BMS;
    if (!bmsChecksumOk(raw, len)) return BMS_PARSE_BAD_CS;

    uint8_t dataLen = raw[5];
    if ((size_t)dataLen + 8 != len) return BMS_PARSE_BAD_LEN;   // 长度字段和实际帧长对不上
    if (dataLen > BMS_MAX_DATA_LEN) return BMS_PARSE_BAD_LEN;   // 数据区比缓冲区还长 → 丢弃, 绝不能拷贝

    uint8_t plain[BMS_MAX_DATA_LEN];
    for (uint8_t i = 0; i < dataLen; i++) {
        plain[i] = (uint8_t)(raw[6 + i] - 0x33);
    }

    if (dataLen == BMS_STATUS_LEN) {
        bmsParseStatus(plain, out);
        return BMS_PARSE_OK;
    }

    if (dataLen == BMS_CELLS_LEN) {
        bmsParseCells(plain, out);
        return BMS_PARSE_OK;
    }

    if (dataLen == BMS_FULL_LEN) {
        // 状态 + 单体电压拼在一帧里 (这台车的 BMS 就是这么回的)
        bmsParseStatus(plain, out);
        bmsParseCells(plain + BMS_STATUS_LEN, out);
        return BMS_PARSE_OK;
    }

    if (dataLen == BMS_MODEL_LEN || dataLen == BMS_INFO_LEN) {
        uint8_t off = (dataLen == BMS_INFO_LEN) ? 7 : 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < BMS_MODEL_CHARS && off + i < dataLen; i++) {
            uint8_t c = plain[off + i];
            if (c < 0x20 || c > 0x7E) break;   // 不是可打印 ASCII 就停
            out->model[n++] = (char)c;
        }
        if (n > 0) {
            out->model[n] = '\0';
            out->hasModel = true;
        }
        return BMS_PARSE_OK;
    }

    return BMS_PARSE_UNKNOWN;
}

#endif
