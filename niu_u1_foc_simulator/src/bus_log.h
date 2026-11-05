/**
 * 485 总线日志 —— 环形缓冲, 记下总线上每一帧 (收到的 + 我们自己回的) 和毫秒时间戳
 *
 * 为什么记在板子上, 而不是记在手机上:
 *   小程序看到的 485 帧是「每 200ms 只推最后一帧」的采样 —— 总线实际约 30 帧/秒,
 *   采样只剩 5 帧/秒, 存下来离线分析会漏掉八成.
 *   而「某帧没来 / 某帧被撞坏」这种问题, 恰恰只能靠日志去找, 采样日志正好把证据丢了.
 *   板子是这条总线上唯一的全知视角: 每个字节它都收得到, 时间戳还是它自己的毫秒时钟.
 *
 * 存二进制 (省 RAM), 拉取的时候再转成文本 (给人看 / 给分析脚本看):
 *   一条记录 = [时间戳 u32][方向 u8][长度 u8][帧数据]
 *   方向: 0 = 总线上收到的 (RX), 1 = 我们自己发出去的 (TX)
 *   文本格式: "RX 12345 68 40 BF 68 02 02 05 08"  (一行一帧, 十六进制大写)
 *
 * 环形缓冲写满了就覆盖最老的 —— 它保存的是「最近 N 帧」, 不是全程录像.
 * 车上的问题基本都是「刚才那一会儿不对」, 最近几十秒够用.
 *
 * 拉日志的时候必须**冻结**: 传输要几秒, 这几秒里总线还在跑, 新帧会写进缓冲把最老的
 * 内容顶掉 —— 传到一半前面的数据就变了, 日志废了. 所以传输期间停止记录,
 * 传完 (或者拉取方不再接了, 超时) 再恢复.
 *
 * 这个头文件不依赖 Arduino, 可以在 PC 上编译跑测试.
 */

#ifndef BUS_LOG_H
#define BUS_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// 一条记录最多存多少字节帧数据.
// 帧长 = 6 + len + 2, len 是 1 字节 → 理论上能到 263;
// 但总线上实际最长的帧是 BMS 的 43 字节信息帧 (整帧 51 字节), 64 够用.
#define BUS_LOG_DATA_MAX  64

// 环形缓冲存多少条记录.
// 总线约 30 帧/秒 → 256 条 ≈ 8~9 秒.
// RAM 占用 256 * 70 = 17.5KB (整片固件现在 DRAM 才用掉 20%, 加这个还在安全范围内).
#define BUS_LOG_SLOTS     256

// 记录方向
#define BUS_LOG_DIR_RX    0
#define BUS_LOG_DIR_TX    1

// 一行文本最长多少字节: "TX 4294967295 " (14) + 64 字节的十六进制 (64*3-1=191) + "\n"
#define BUS_LOG_TEXT_MAX  224

class BusLog {
public:
    void begin() {
        head_ = 0;
        count_ = 0;
        frozen_ = false;
    }

    // 记一条帧. dir: BUS_LOG_DIR_RX / BUS_LOG_DIR_TX, tMs: 毫秒时间戳
    void record(uint8_t dir, uint32_t tMs, const uint8_t* data, size_t len) {
        if (frozen_) return;              // 传输期间冻结
        if (!data || len == 0) return;
        if (len > BUS_LOG_DATA_MAX) len = BUS_LOG_DATA_MAX;
        Slot& s = slots_[head_];
        s.t = tMs;
        s.dir = dir;
        s.len = (uint8_t)len;
        memcpy(s.data, data, len);
        head_ = (uint16_t)((head_ + 1) % BUS_LOG_SLOTS);
        if (count_ < BUS_LOG_SLOTS) count_++;
    }

    // 有效记录数 (最多 BUS_LOG_SLOTS)
    uint16_t count() const { return count_; }

    bool frozen() const { return frozen_; }
    void freeze(bool on) { frozen_ = on; }

    // 按时间顺序取第 i 条 (i = 0 是最老的一条)
    bool get(uint16_t i, uint32_t* t, uint8_t* dir, const uint8_t** data, uint8_t* len) const {
        if (i >= count_) return false;
        uint16_t idx = (uint16_t)((head_ + BUS_LOG_SLOTS - count_ + i) % BUS_LOG_SLOTS);
        const Slot& s = slots_[idx];
        *t = s.t;
        *dir = s.dir;
        *data = s.data;
        *len = s.len;
        return true;
    }

    // 整份日志转成文本的总长度 (不含结尾 0) —— 用来给手机显示进度条
    uint32_t totalTextLen() const {
        uint32_t total = 0;
        for (uint16_t i = 0; i < count_; i++) {
            uint32_t t; uint8_t dir; const uint8_t* data; uint8_t len;
            if (!get(i, &t, &dir, &data, &len)) break;
            total += (uint32_t)recordTextLen(t, dir, len);
        }
        return total;
    }

    // 一条记录转成文本后的长度 (不含结尾 0)
    static size_t recordTextLen(uint32_t t, uint8_t dir, uint8_t len) {
        // "TX " (3) + 十进制时间戳 + " " (1) + 十六进制 (3*len-1) + "\n" (1)
        return 4 + digitCount(t) + 3 * (size_t)len;
    }

    // 一条记录转成一行文本 (结尾带 '\n'). 返回写入的字节数 (不含结尾 0)
    static size_t formatRecord(char* out, size_t cap, uint32_t t, uint8_t dir,
                               const uint8_t* data, uint8_t len) {
        size_t p = 0;
        int n = snprintf(out, cap, "%s %lu ", (dir == BUS_LOG_DIR_TX) ? "TX" : "RX",
                         (unsigned long)t);
        if (n < 0) return 0;
        p = (size_t)n;
        for (uint8_t i = 0; i < len && p + 4 < cap; i++) {
            n = snprintf(out + p, cap - p, "%s%02X", i ? " " : "", data[i]);
            if (n < 0) break;
            p += (size_t)n;
        }
        if (p + 1 < cap) {
            out[p++] = '\n';
            out[p] = '\0';
        }
        return p;
    }

private:
    struct Slot {
        uint32_t t;
        uint8_t  dir;
        uint8_t  len;
        uint8_t  data[BUS_LOG_DATA_MAX];
    };

    static size_t digitCount(uint32_t v) {
        size_t n = 1;
        while (v >= 10) { v /= 10; n++; }
        return n;
    }

    Slot    slots_[BUS_LOG_SLOTS];
    uint16_t head_ = 0;      // 下一个写进去的位置
    uint16_t count_ = 0;     // 有效记录数 (≤ BUS_LOG_SLOTS)
    bool     frozen_ = false;
};

// 单例 (函数内 static, 保证整个程序只有一份)
inline BusLog& busLog() {
    static BusLog inst;
    return inst;
}

/**
 * 把环形缓冲里的记录变成文本, 按块吐出来.
 *
 * 单独拆成这个类是为了能在 PC 上编译跑测试 —— 固件里"日志 → 蓝牙包"这一段
 * 最容易出的错是**固件和小程序两边格式对不上** (偏移算错一个字节, 整份日志就废了),
 * 而这段逻辑不连板子也能测.
 *
 * 用法:
 *     BusLogWriter w;
 *     w.begin(0);                       // 从头开始
 *     bool done = false;
 *     size_t n = w.next(buf, sizeof(buf), &done);
 */
class BusLogWriter {
public:
    // 开始生成. offset > 0 = 跳过前 offset 个字节 (断点续传: 手机发现中间丢了一块,
    // 让设备从这里接着给). 环形缓冲在传输期间是冻结的, 所以重新生成的文本跟
    // 刚才那一遍一模一样, 偏移对得上.
    void begin(uint32_t offset) {
        recIdx_ = 0;
        recLeft_ = 0;
        recLen_ = 0;
        skip_ = offset;
        total_ = busLog().totalTextLen();
        if (skip_ > total_) skip_ = total_;
    }

    uint32_t total() const { return total_; }
    uint32_t skip() const { return skip_; }

    // 吐出下一块文本. done = true 表示这是最后一块.
    // 返回写入 out 的字节数.
    size_t next(char* out, size_t cap, bool* done) {
        size_t n = 0;
        *done = false;
        while (n < cap) {
            if (recLeft_ == 0) {
                uint32_t t; uint8_t dir; const uint8_t* data; uint8_t len;
                if (!busLog().get(recIdx_, &t, &dir, &data, &len)) {
                    *done = true;
                    break;
                }
                recIdx_++;
                recLen_ = BusLog::formatRecord(rec_, sizeof(rec_), t, dir, data, len);
                recLeft_ = recLen_;
                if (recLeft_ == 0) continue;
            }
            // 断点续传: 先把要跳过的字节吃掉 (跳过的那段不是输出的一部分)
            if (skip_ > 0) {
                size_t s = (skip_ < recLeft_) ? (size_t)skip_ : recLeft_;
                memmove(rec_, rec_ + s, recLeft_ - s);
                recLeft_ -= s;
                skip_ -= (uint32_t)s;
                continue;
            }
            size_t take = cap - n;
            if (take > recLeft_) take = recLeft_;
            memcpy(out + n, rec_, take);
            memmove(rec_, rec_ + take, recLeft_ - take);
            recLeft_ -= take;
            n += take;
        }
        return n;
    }

private:
    uint32_t recIdx_ = 0;
    size_t   recLeft_ = 0;
    size_t   recLen_ = 0;
    uint32_t skip_ = 0;
    uint32_t total_ = 0;
    char     rec_[BUS_LOG_TEXT_MAX];
};

#endif
