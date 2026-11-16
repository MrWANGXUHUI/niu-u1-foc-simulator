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
// 2026-11-16 重定标: 这台车的最高速度是 **36km/h** (之前注释里写的 40 是错的)
// ---------------------------------------------------------------------------
//
// 用户上车确认: 车最高只到 36km/h. 于是最短真间隔 = 1.28m × 3.6 / 36
// = **128ms** (不是之前按 40km/h 算的 115ms). 所有门槛都从 128ms 重新推:
//
// 1) **毛刺闸门地板 = 100ms** (HALL_MIN_INTERVAL_US)
//
//    - 必须 **> 振铃偏移上限 60ms** (车上实测干扰是"离真沿 3~60ms")
//    - 必须 **< 最短真间隔 128ms** (36km/h), 否则真脉冲会被误伤
//    - 100ms 落在 (60, 128) 正中间: 离振铃 40ms, 离真间隔 28ms (22% 余量)
//    - 附带好处: **能报出来的车速上限 = 1.28 × 3.6 / 0.100 = 46km/h**.
//      也就是说"刚骑车飙到一百多"这种读数从此**结构上不可能出现**
//      (25ms 地板时上限是 184km/h, 什么离谱值都可能出现).
//
// 2) **闸门上限 = 起步门槛 = 100ms** —— 三个数一样, 闸门就是**常数 100ms**
//
//    自适应那档 (参照量 × 60%) 的作用是"挡掉比真周期短 40% 的振铃".
//    地板抬到 100ms 以后, 60ms 以内的振铃**本来就被地板挡掉了**, 自适应项
//    变成死代码. 好处比"少一个参数"大得多: **门槛不再会自己往下掉**.
//    1.5.4 那个"车停着充电报 61km/h"的根因就是:
//        参照量被毛刺拉低 → 门槛跟着低 → 更多毛刺进得来 → 参照量更低 …
//    (自持的下行失控). 门槛是常数, 这个环路就不存在了.
//
// 3) **判据是"距上一个被接受的脉冲", 不是"距上一个物理脉冲"**
//
//    "每个物理沿都更新基准"听起来更严, 实际上会**在 36km/h 直接测不出车速**:
//
//      1. 车上实测的干扰是"离真沿 3~60ms" —— 每个真沿后面跟着一串振铃沿
//      2. 36km/h 的真间隔是 128ms
//      3. 最后一个振铃沿在真沿后 60ms → 下一个真沿离它只有 128-60 = **68ms**
//      4. 门槛 70ms → 68 < 70 → **真沿被当成毛刺丢掉**
//      5. 真沿被丢掉 → 这一拍量到的间隔翻倍 → 车速显示一半 (18 而不是 36)
//      6. 越骑越快, 间隔越短, 丢得越多 → 高速时整段读数是 0
//
//    (tools/test_hall_speed.cpp 场景 12 复现的就是这个: 36km/h + 振铃 → 0.0km/h)
//
//    所以判据只能用"距上一个**被接受**的脉冲": 振铃沿离真沿只有 3~60ms,
//    被门槛 (100ms) 挡掉, 且**不动** last/prev 时间戳 → 下一个真沿离
//    "上一个被接受的脉冲"整整 128ms → 稳稳收下.
//
// ---------------------------------------------------------------------------
// 三个关键设计 (改任何一个都要重跑 tools/test_hall_speed.cpp)
// ---------------------------------------------------------------------------
//
// 1) **丢毛刺时不更新时间戳**.
//    丢掉时如果把 last/prev 往前挪, 一个毛刺就把一个真间隔劈成"几毫秒 + 剩下的"
//    两半 —— 短的进历史, 长的比真实值大, 车速两头都错.
//
// 2) **门槛 = 常数 100ms** (地板 = 上限 = 起步值, 见上面第 2 条).
//
// 3) **车速取历史的中位数** —— 漏一个脉冲 (5 个里 1 个是 2 倍) 不影响中位数,
//    毛刺劈出的两个半间隔也一样.
//
// ---------------------------------------------------------------------------
// 已知没解决的问题 (2026-11-16)
// ---------------------------------------------------------------------------
//
// **车停着不动时, 噪声脉冲仍可能被当成真脉冲**, 于是报出一个假车速.
// 2026-11-11 真车日志: 车停着充电, 固件连续 11.5 秒报协议值 127
// (= 61km/h 封顶, 说明内部间隔 <= 75.5ms), 然后才归零.
//
// 抬到 100ms 地板以后, 这个假读数被压到 <= 46km/h, 但**没有归零**:
// 停着的车和一个真在转的轮子, 间隔落在同一个区间 (100~200ms),
// 软件**分不出来** —— 只有"噪声本身规律性够差"才能靠一致性判定挡掉,
// 而那个判定会误伤滑行减速 (间隔本来就在变大).
//
// 真正的解法是硬件: 霍尔线是长线 + ESP32 内部 45k 上拉, 抗干扰能力太差.
// 建议外挂 **4.7k~10k 上拉 + 100nF 电容到 GND**, 并让霍尔线远离 485 线束.
// (docs/niu485-20261111-090152.log 里的假车速周期 ~71ms, 跟 485 总线的
//  帧率 (14.1 帧/秒 = 71ms/帧) 几乎一样 —— 怀疑噪声就是我们自己的总线
//  活动耦合进霍尔线的. 用串口日志里的"原始间隔"可以验证, 见下面 phys_gap_us.)
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
// 毛刺闸门**地板**: 比这更快的间隔一律当毛刺丢掉.
//
// 100ms 的依据 (三条约束夹出来的, 详见文件头):
//   - **> 振铃偏移上限 60ms** —— 实测车上干扰是"离真沿 3~60ms", 26~60ms
//     的振铃全被挡掉 ✓
//   - **< 最短真间隔 128ms** —— 这车最高 36km/h, 真脉冲不会被误伤 ✓
//   - 附带: 被接受的间隔 >= 100ms → 报出来的车速 <= 1.28m/0.100s = 46km/h,
//     "飙到一百多"从此结构上不可能出现 ✓
//
// ⚠️ 别再往下调 (25ms/70ms 都试过):
//    25ms → 上限 184km/h, 而且自适应项能把门槛拉到 25ms 地板 → 假脉冲进历史
//           → 参照量跟着掉 → 门槛更松 → 更多假脉冲进来 (自持的下行失控)
//    70ms → 判据改成"距上一个物理脉冲"后, 36km/h 时真沿离上一个振铃沿只有
//           68ms < 70ms → **真沿被丢掉 → 高速时读数直接是 0**
#define HALL_MIN_INTERVAL_US 100000  // 毛刺闸门地板 (见上面说明)
#endif
#ifndef HALL_GATE_MAX_US
#define HALL_GATE_MAX_US 100000      // 门槛上限: 跟地板一样 → 闸门是常数 100ms
#endif
#ifndef HALL_GATE_STARTUP_US
#define HALL_GATE_STARTUP_US 100000  // 起步门槛 (还没有参照量时用): 同样是 100ms
#endif
#ifndef HALL_STOP_TIMEOUT_US
// 停转阈值 3s 是为了顾到低速骑行: 3s 转一圈 = 1.28m/3s = 1.5 km/h,
// 低于 1.5km/h 仪表本来就看不出差别, 归零没有损失; 而 2s 的话
// 3~5km/h 的"蠕行"会整段显示 0 (间隔 1.5~2.3s 正好卡在阈值上).
#define HALL_STOP_TIMEOUT_US 3000000 // 3s 没有新脉冲 → 停转, 车速归 0
#endif

// 轮速换算需要的轮子参数 (固件里以这两个为准)
#ifndef HALL_WHEEL_CIRCUMFERENCE_M
#define HALL_WHEEL_CIRCUMFERENCE_M 1.28
#endif
#ifndef HALL_MAGNET_COUNT
#define HALL_MAGNET_COUNT 1
#endif

// 诊断用: 记住最近几个**物理**间隔 (收没收都记), 用来判断噪声长什么样
#ifndef HALL_RAW_GAP_N
#define HALL_RAW_GAP_N 8
#endif

struct HallSpeed {
    // ---------- ISR 侧 (中断里改) ----------
    volatile uint32_t last_pulse_us = 0;  // 最近一个**被接受**的脉冲
    volatile uint32_t prev_pulse_us = 0;  // 上一个被接受的脉冲
    volatile uint32_t pulse_count = 0;    // 被接受的脉冲总数
    volatile uint32_t glitch_count = 0;   // 被判为毛刺丢掉的脉冲数 (诊断用)
    volatile uint32_t phys_count = 0;     // 物理脉冲总数 (含被丢掉的) (诊断用)
    volatile uint32_t last_phys_us = 0;   // 最近一个物理脉冲 (**只用于诊断**, 不参与测速)

    // 诊断: 最近 HALL_RAW_GAP_N 个**物理**间隔 (含被丢掉的毛刺).
    // 用途: "车停着但一直有脉冲"时打印这个, 就能看出噪声的周期结构
    // (是 485 总线帧率 71ms? 还是充电器的开关噪声?).
    volatile uint32_t phys_gap_us[HALL_RAW_GAP_N] = {0};
    volatile uint8_t phys_gap_idx = 0;

    // ---------- 主循环侧 ----------
    uint32_t hist_us[HALL_HISTORY] = {0}; // 最近几个被接受的间隔 (环形)
    uint8_t hist_cnt = 0;
    uint8_t hist_idx = 0;
    uint32_t used_pulse_us = 0;    // 已经算过间隔的那个脉冲时刻
    uint32_t last_interval_us = 0; // 最近一次测到的间隔 (只用于日志)
    uint32_t median_us = 0;       // 算车速实际用的那个间隔 (中位数)
    uint32_t gap_reset = 0;       // 因停车太久重置过历史的次数 (诊断用)
    volatile uint32_t ref_us = 0; // 闸门参照量: 历史间隔的中位数 (主循环算好发布给 ISR)

    // 闸门门槛: clamp(参照量 × 60%, 地板, 上限). 见上面第 2 条.
    //
    // ⚠️ 现在地板 = 上限 = 起步值 = 100ms, 所以**门槛是常数 100ms**:
    //    ① 丢得掉的只有"离上一个被接受的脉冲 < 100ms"的脉冲 —— 车上实测的
    //       振铃 (离真沿 3~60ms) 全在这里 ✓
    //    ② 门槛不会因为参照量变小而变松 —— 1.5.4 "停车报 61km/h"的自持
    //       下行失控环路被切断了 ✓
    //    ③ 参照量那个 ×60% 的自适应项在当前参数下是死代码 (floor == max),
    //       留着是为了以后改参数时有个明确的锚点.
    HALL_SPEED_IRAM uint32_t gateUs() const {
        uint32_t ref = ref_us;
        uint32_t min_ok;
        if (ref == 0) {
            min_ok = HALL_GATE_STARTUP_US;    // 不知道周期 → 用同一个保守值
        } else {
            min_ok = HALL_MIN_INTERVAL_US;
            uint32_t adaptive = ref * 3 / 5;
            if (adaptive > min_ok) min_ok = adaptive;
            if (min_ok > HALL_GATE_MAX_US) min_ok = HALL_GATE_MAX_US;
        }
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
        // 1ms 消抖: 单磁钢在 30km/h 下脉冲间隔约 154ms, 1ms 阈值不会漏计真实脉冲.
        // 用 last_phys_us 做基准 —— 挤在一起的一串毛刺要靠这个消抖掉, 否则它们
        // 会被一个个当成"物理脉冲"记进诊断里 (物理计数就跟真实边沿对不上了).
        if (now_us - last_phys_us <= HALL_DEBOUNCE_US) return;

        // 诊断: 物理脉冲 (收没收都记) 和它跟上一个物理脉冲的间隔.
        uint32_t prev_phys = last_phys_us;
        last_phys_us = now_us;
        phys_count++;
        if (prev_phys != 0) {
            phys_gap_us[phys_gap_idx] = now_us - prev_phys;
            phys_gap_idx = (uint8_t)((phys_gap_idx + 1) % HALL_RAW_GAP_N);
        }

        // 物理上不可能的快脉冲 → 判为毛刺 (振铃/干扰), 丢掉.
        // 关键: **丢掉时不更新 last_pulse_us / prev_pulse_us** (见上面第 1 条).
        //
        // ⚠️ 判据是"距上一个**被接受**的脉冲", 不是"距上一个物理脉冲" ——
        //    用物理脉冲当基准的话, 36km/h 时真沿离上一个振铃沿只有 68ms,
        //    门槛 70ms 就把真沿当毛刺丢了 → 高速时读数直接是 0.
        //    详见文件头的第 3 条.
        uint32_t gap = now_us - last_pulse_us;
        uint32_t min_ok = gateUs();
        if (last_pulse_us != 0 && gap < min_ok) {
            glitch_count++;
            return;
        }
        prev_pulse_us = last_pulse_us;
        last_pulse_us = now_us;
        pulse_count++;
    }

    // 最近几个物理间隔 (诊断用, 主循环读)
    void rawGaps(uint32_t* out, uint8_t n) const {
        uint8_t cnt = 0;
        uint8_t idx = phys_gap_idx;           // 下一个要写的 = 最老的
        for (uint8_t i = 0; i < n; i++) {
            idx = (uint8_t)((idx + HALL_RAW_GAP_N - 1) % HALL_RAW_GAP_N);
            out[cnt++] = phys_gap_us[idx];
        }
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
