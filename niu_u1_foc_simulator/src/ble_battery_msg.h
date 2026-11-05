/**
 * 电池信息在 BLE 上的打包格式 (纯 C++, 不依赖 Arduino)
 *
 * 一条特征 (6e400004) 上跑多种消息, 每条通知带 3 字节头:
 *
 *   [0] 消息类型   1=状态 2=单体电压 3=型号串
 *   [1] 分片序号   从 0 开始
 *   [2] 分片总数
 *   [3..] 数据
 *
 * 分片是因为蓝牙协商不出 MTU 时单条通知最多 20 字节, 而 13 串单体电压就有 26 字节.
 *
 * 状态消息 (17 字节):
 *   [0]     标志, bit0 = 有数据
 *   [1]     电量 %
 *   [2..3]  总电压 0.1V (小端 u16)
 *   [4..5]  电流 0.1A (小端 i16)
 *   [6]     温度个数
 *   [7..11] 5 个温度 (i8, °C)
 *   [12]    单体串数
 *   [13..14] 最低单体 mV (小端 u16)
 *   [15..16] 最高单体 mV (小端 u16)
 *
 * 诊断消息 (16 字节) —— "为什么没收到电池报文"的答案:
 *   [0..1]  总线上看到的 BMS 帧数 (u16)
 *   [2..3]  解析成功数 (u16)
 *   [4]     校验和错
 *   [5]     长度对不上
 *   [6]     数据区长度没见过
 *   [7]     上次看到的 BMS 帧的总长度
 *   [8..11] 距上次 BMS 帧过了多少毫秒 (u32, 0xFFFFFFFF = 从来没有)
 *   [12..13] V35 查询数 (u16)
 *   [14..15] 我们的应答数 (u16)
 *
 * 单体电压消息: 每格 2 字节 mV (小端 u16)
 * 型号串消息: ASCII
 *
 * 单独拆成这个头文件是为了能在 PC 上编译跑测试 —— 把真车抓包里的 BMS 报文
 * 解出来, 用这里的函数打包, 再喂给小程序端的解析代码, 两边对得上才算数.
 */

#ifndef BLE_BATTERY_MSG_H
#define BLE_BATTERY_MSG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bms_protocol.h"

#define BLE_BMSG_STATUS  1
#define BLE_BMSG_CELLS   2
#define BLE_BMSG_MODEL   3
#define BLE_BMSG_DIAG    4

// 单条通知里除消息头外最多带多少字节数据 (20 - 3)
#define BLE_BMSG_PAYLOAD_MAX 17
// 单条通知的总长度上限
#define BLE_BMSG_CHUNK_MAX (3 + BLE_BMSG_PAYLOAD_MAX)

// 状态消息的数据长度
#define BLE_BMSG_STATUS_LEN 17
// 诊断消息的数据长度
#define BLE_BMSG_DIAG_LEN  16

// 打包状态消息, 返回写入的字节数 (固定 BLE_BMSG_STATUS_LEN)
static inline size_t bleBatteryPackStatus(const BmsInfo* info, uint8_t* out) {
    uint16_t minMv = 0, maxMv = 0;
    if (info->cellCount > 0) {
        minMv = 0xFFFF;
        for (uint8_t i = 0; i < info->cellCount; i++) {
            if (info->cellMv[i] < minMv) minMv = info->cellMv[i];
            if (info->cellMv[i] > maxMv) maxMv = info->cellMv[i];
        }
    }
    out[0]  = 0x01;                                   // 有状态数据
    out[1]  = info->soc;
    out[2]  = (uint8_t)(info->voltage_dv & 0xFF);
    out[3]  = (uint8_t)(info->voltage_dv >> 8);
    int16_t cur_da = (int16_t)(info->current * 10);  // BMS 给整数 A, 放大 10 倍留小数位
    out[4]  = (uint8_t)(cur_da & 0xFF);
    out[5]  = (uint8_t)((uint16_t)cur_da >> 8);
    out[6]  = info->tempCount;
    for (uint8_t i = 0; i < BMS_TEMP_COUNT; i++) {
        out[7 + i] = (uint8_t)info->temps[i];
    }
    out[12] = info->cellCount;
    out[13] = (uint8_t)(minMv & 0xFF);
    out[14] = (uint8_t)(minMv >> 8);
    out[15] = (uint8_t)(maxMv & 0xFF);
    out[16] = (uint8_t)(maxMv >> 8);
    return BLE_BMSG_STATUS_LEN;
}

// 打包单体电压消息, 返回字节数 (每格 2 字节)
static inline size_t bleBatteryPackCells(const BmsInfo* info, uint8_t* out) {
    for (uint8_t i = 0; i < info->cellCount; i++) {
        out[i * 2] = (uint8_t)(info->cellMv[i] & 0xFF);
        out[i * 2 + 1] = (uint8_t)(info->cellMv[i] >> 8);
    }
    return (size_t)info->cellCount * 2;
}

// 打包型号串消息, 返回字节数
static inline size_t bleBatteryPackModel(const BmsInfo* info, uint8_t* out) {
    size_t n = strlen(info->model);
    memcpy(out, info->model, n);
    return n;
}

// 打包诊断消息, 返回字节数 (固定 BLE_BMSG_DIAG_LEN)
static inline size_t bleBatteryPackDiag(const BmsDiag* d, uint8_t* out) {
    out[0] = (uint8_t)(d->rxFrames & 0xFF);
    out[1] = (uint8_t)(d->rxFrames >> 8);
    out[2] = (uint8_t)(d->okFrames & 0xFF);
    out[3] = (uint8_t)(d->okFrames >> 8);
    out[4] = d->csErr;
    out[5] = d->lenErr;
    out[6] = d->unknownLen;
    out[7] = d->lastLen;
    out[8] = (uint8_t)(d->ageMs & 0xFF);
    out[9] = (uint8_t)((d->ageMs >> 8) & 0xFF);
    out[10] = (uint8_t)((d->ageMs >> 16) & 0xFF);
    out[11] = (uint8_t)((d->ageMs >> 24) & 0xFF);
    out[12] = (uint8_t)(d->qCount & 0xFF);
    out[13] = (uint8_t)(d->qCount >> 8);
    out[14] = (uint8_t)(d->aCount & 0xFF);
    out[15] = (uint8_t)(d->aCount >> 8);
    return BLE_BMSG_DIAG_LEN;
}

// 把一条消息切成一片. 返回这一片的总长度 (3 + 数据), 数据为空时返回 0
static inline size_t bleBatteryPackChunk(uint8_t type, const uint8_t* payload, size_t len,
                                         uint8_t chunkIdx, uint8_t* out) {
    size_t off = chunkIdx * BLE_BMSG_PAYLOAD_MAX;
    if (off >= len) return 0;
    size_t n = len - off;
    if (n > BLE_BMSG_PAYLOAD_MAX) n = BLE_BMSG_PAYLOAD_MAX;
    size_t total = (len + BLE_BMSG_PAYLOAD_MAX - 1) / BLE_BMSG_PAYLOAD_MAX;
    out[0] = type;
    out[1] = (uint8_t)chunkIdx;
    out[2] = (uint8_t)total;
    memcpy(out + 3, payload + off, n);
    return 3 + n;
}

#endif
