// 霍尔测速的"间隔测量 + 毛刺闸门"逻辑.
//
// 单独抽成头文件 (不依赖 Arduino) 是为了能在 PC 上做单元测试 ——
// 车速"冻在 19.6km/h" 这个坑就是靠 tools/test_hall_speed.cpp 复现出来的.
// ⚠️ 固件里不要另写一份, 直接用这个头文件, 否则测的就不是车上的代码了.
//
// 术语:
//   脉冲 = 磁钢经过霍尔传感器一次 (1 磁钢, 转一圈一个脉冲)
//   间隔 = 相邻两个**被接受**的脉冲之间的时间
//   毛刺 = 传感器振铃 / 线受干扰产生的多余脉冲 (车上实测: 离真沿 3~60ms)
//
// ---------------------------------------------------------------------------
// 三个关键设计 (改任何一个都要重跑 tools/test_hall_speed.cpp)
// ---------------------------------------------------------------------------
//
// 1) **丢毛刺时不更新时间戳**.
//    丢掉时如果把 last/prev 往前挪, 一个毛刺就把一个真间隔劈成"几毫秒 + 剩下的"
//    两半 —— 短的进历史, 长的比真实值大, 车速两头都错.
//
// 2) **门槛 = 绝对下限, 再按参照量收紧, 但有上限**.
//
//      门槛 = clamp(参照量 × 60%, 25ms, 80ms)
//
//    a) 下限 25ms —— 物理上不可能的快脉冲 (传感器振铃 3~6ms).
//
//    b) 参照量 × 60% —— 自适应, 挡"离真沿 50~60ms"那种振铃.
//       参照量取历史里的**中位数**: 中位数天生抗离群值, 一个毛刺混进历史
//       也拉不动它.
//       (不能用"最近一个": 那个值会被毛刺带跑, 门槛越算越松, 毛刺越放越进来;
//        也不能用"最小值": 门槛会被一个毛刺拉到 25ms 的地板, 之后所有毛刺
//        都进得来 —— 恶性循环, 车速跳来跳去. 这两种都试过, 见 NOTES.md §6.9.)
//
//    c) **上限 80ms** —— 修"车速冻在 19.6km/h"的关键, 也是最容易被忽略的一条.
//
//       门槛一旦超过真脉冲可能的最小间隔, 就会开始丢**真**脉冲, 而且是自锁的:
//         真脉冲被当成毛刺丢掉 (丢掉时不更新时间戳)
//           → 这一拍量到的间隔翻倍 (跨了两圈)
//           → 参照量跟着翻倍 → 门槛也跟着涨
//           → 真脉冲还是比门槛短 → 还是被丢 → 越死越死
//       结果: 车速永久锁死在真实值的**一半**, 除非停下来.
//
//       19.6km/h 正好是 39.2km/h 的一半 —— 这就是用户在车上看到的那个数.
//       触发场景: 19.6km/h 时一脚油门冲到 40km/h (真间隔 235ms → 115ms,
//       一步掉了 51%), 第一个真脉冲就被门槛挡掉了.
//
//       上限的意义: **门槛永远不超过 80ms, 而 40km/h 的真间隔是 115ms**,
//       所以真脉冲永远不会被误伤, 锁死从"可能发生"变成"结构上不可能".
//       80ms 同时高于实测的振铃偏移上限 (60ms), 所以毛刺照样被挡 ✓
//       80ms 对应 1.28m 轮子的 57.6km/h —— 这台车最高 40km/h, 余量够.
//
// 3) **车速取历史的中位数** —— 漏一个脉冲 (5 个里 1 个是 2 倍) 不影响中位数,
//    毛刺劈出的两个半间隔也一样.
//
// ---------------------------------------------------------------------------

#ifndef HALL_SPEED_H
#define HALL_SPEED_H

#include <stdint.h>

// ISR 里的函数要放进 IRAM —— OTA 写 flash 时会关掉 flash cache,
// 此时从中断里调 flash 里的代码会崩. PC 上编译时没有这个宏, 留空即可.
#ifdef ARDUINO
#include <esp_attr.h>
#define HALL_SPEED_IRAM IRAM_ATTR
#else
#define HALL_SPEED_IRAM
#endif

// 固件 (.ino) 里已经定义过这几个常量时, 沿用固件的值 ——
// 两边必须一致, 否则测的就不是车上的代码. 没定义时用这里的默认值.
#ifndef HALL_HISTORY
#define HALL_HISTORY 5               // 历史里保留几个间隔 (取中位数用)
#endif
#ifndef HALL_DEBOUNCE_US
#define HALL_DEBOUNCE_US 1000        // ISR 消抖: 1ms
#endif
#ifndef HALL_MIN_INTERVAL_US
#define HALL_MIN_INTERVAL_US 25000   // 绝对下限: 物理上不可能的快脉冲
#endif
#ifndef HALL_GATE_MAX_US
#define HALL_GATE_MAX_US 80000       // 门槛上限: 见上面第 2c 条 —— 修"冻在 19.6"的键
#endif
#ifndef HALL_STOP_TIMEOUT_US
#define HALL_STOP_TIMEOUT_US 3000000 // 3s 没有新脉冲 → 停转, 车速归 0
#endif

// 车速换算需要的轮子参数 (固件里以这两个为准)
#ifndef HALL_WHEEL_CIRCUMFERENCE_M
#define HALL_WHEEL_CIRCUMFERENCE_M 1.28
#endif
#ifndef HALL_MAGNET_COUNT
#define HALL_MAGNET_COUNT 1
#endif

struct HallSpeed {
    // ---------- ISR 侧 (中断里改) ----------
    volatile uint32_t last_pulse_us = 0;  // 最近一个**被接受**的脉冲
    volatile uint32_t prev_pulse_us = 0;  // 上一个被接受的脉冲
    volatile uint32_t pulse_count = 0;    // 被接受的脉冲总数
    volatile uint32_t glitch_count = 0;   // 被判为毛刺丢掉的脉冲数 (诊断用)

    // ---------- 主循环侧 ----------
    uint32_t hist_us[HALL_HISTORY] = {0}; // 最近几个被接受的间隔 (环形)
    uint8_t hist_cnt = 0;
    uint8_t hist_idx = 0;
    uint32_t used_pulse_us = 0;    // 已经算过间隔的那个脉冲时刻
    uint32_t last_interval_us = 0; // 最近一次测到的间隔 (只用于日志)
    uint32_t median_us = 0;       // 算车速实际用的那个间隔 (中位数)
    uint32_t gap_reset = 0;       // 因停车太久重置过历史的次数 (诊断用)
    volatile uint32_t ref_us = 0; // 闸门参照量: 历史间隔的中位数 (主循环算好发布给 ISR)

    // 闸门门槛: clamp(参照量 × 60%, 绝对下限, 上限). 见上面第 2 条.
    HALL_SPEED_IRAM uint32_t gateUs() const {
        uint32_t min_ok = HALL_MIN_INTERVAL_US;
        uint32_t ref = ref_us;
        if (ref > 0) {
            uint32_t adaptive = ref * 3 / 5;
            if (adaptive > min_ok) min_ok = adaptive;
        }
        if (min_ok > HALL_GATE_MAX_US) min_ok = HALL_GATE_MAX_US;
        return min_ok;
    }

    // 历史里的中位数 (车速用 + 闸门参照量). 天生抗离群值.
    // 环形的下标顺序是乱的, 但中位数不看顺序 ✓
    uint32_t histMedian() const {
        if (hist_cnt == 0) return 0;
        uint32_t tmp[HALL_HISTORY];
        for (uint8_t i = 0; i < hist_cnt; i++) tmp[i] = hist_us[i];
        for (uint8_t i = 1; i < hist_cnt; i++) {
            uint32_t key = tmp[i];
            int8_t j = (int8_t)i - 1;
            while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = key;
        }
        return tmp[hist_cnt / 2];
    }

    void histReset() {
        hist_cnt = 0;
        hist_idx = 0;
        ref_us = 0;
        median_us = 0;
    }

    void histPush(uint32_t interval) {
        hist_us[hist_idx] = interval;
        hist_idx = (uint8_t)((hist_idx + 1) % HALL_HISTORY);
        if (hist_cnt < HALL_HISTORY) hist_cnt++;
        median_us = histMedian();   // 车速用它
        ref_us = median_us;         // 闸门参照量也是它 (抗离群)
    }

    // ---------- ISR: 来一个脉冲 ----------
    HALL_SPEED_IRAM void onPulse(uint32_t now_us) {
        // 1ms 消抖: 单磁钢在 30km/h 下脉冲间隔约 154ms, 1ms 阈值不会漏计真实脉冲
        if (now_us - last_pulse_us <= HALL_DEBOUNCE_US) return;

        // 物理上不可能的快脉冲 → 判为毛刺 (振铃/干扰), 丢掉.
        // 关键: **丢掉时不更新 last_pulse_us / prev_pulse_us** (见上面第 1 条).
        uint32_t min_ok = gateUs();
        if (last_pulse_us != 0 && now_us - last_pulse_us < min_ok) {
            glitch_count++;
            return;
        }
        prev_pulse_us = last_pulse_us;
        last_pulse_us = now_us;
        pulse_count++;
    }

    // ---------- 主循环: 每 100ms 调一次 ----------
    // 有新的间隔就更新历史; 返回 true = 这次有新间隔.
    bool poll(uint32_t now_us) {
        uint32_t last_us = last_pulse_us;
        uint32_t prev_us = prev_pulse_us;
        if (last_us == used_pulse_us || prev_us == 0 || last_us <= prev_us) return false;
        used_pulse_us = last_us;

        uint32_t interval = last_us - prev_us;
        if (interval >= HALL_STOP_TIMEOUT_US) {
            // 间隔里含了停车时间 (停了很久又起步), 这一拍速度不可信:
            // 历史全部作废, 车速按 0 起算, 让下一拍从干净状态重新收敛.
            // 参照量也要清掉 —— 否则起步那一段(间隔越来越短)会被
            // 旧的参照量拒掉, 车速又冻住 (跟上面那个坑一模一样)
            histReset();
            gap_reset++;
            return false;
        }

        // 第二道闸门 (自适应). ISR 里那道已经挡过一遍了, 这里再挡一次 ——
        // 双保险, 免得毛刺混进历史.
        uint32_t min_ok = gateUs();
        if (interval < min_ok) {
            glitch_count++;
            return false;
        }
        histPush(interval);
        last_interval_us = interval;
        return true;
    }

    // 车速 (km/h) + 转速 (rpm). 停转时返回 0.
    void speed(uint32_t now_us, float* kmh, float* rpm) {
        *kmh = 0.0f;
        *rpm = 0.0f;
        uint32_t last_us = last_pulse_us;
        if (last_us == 0) return;                                            // 从来没收到脉冲
        if ((uint32_t)(now_us - last_us) >= HALL_STOP_TIMEOUT_US) return;    // 停转

        uint32_t base = median_us;
        if (base == 0) return;
        float period_s = base / 1000000.0f;
        *rpm = 60.0f / (period_s * HALL_MAGNET_COUNT);
        *kmh = *rpm * HALL_WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;
    }

    // 只要车速 (诊断 / 测试用)
    float kmh(uint32_t now_us) const {
        uint32_t last_us = last_pulse_us;
        if (last_us == 0) return 0.0f;
        if ((uint32_t)(now_us - last_us) >= HALL_STOP_TIMEOUT_US) return 0.0f;
        uint32_t base = median_us;
        if (base == 0) return 0.0f;
        float period_s = base / 1000000.0f;
        float rpm = 60.0f / (period_s * HALL_MAGNET_COUNT);
        return rpm * HALL_WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;
    }
};

#endif // HALL_SPEED_H
