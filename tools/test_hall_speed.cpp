// 霍尔测速的单元测试 —— 复现"车速冻在 19.6km/h"并证明修好了.
//
//   g++ -Wall -I../niu_u1_foc_simulator/src -o test_hall_speed test_hall_speed.cpp
//   ./test_hall_speed
//
// 测什么:
//   1) 1.5.2 的老逻辑: 加速到 40km/h 会不会锁死在 19.6km/h (复现 bug)
//   2) 新逻辑: 同样的加速过程, 车速能不能跟上去
//   3) 毛刺 (振铃) 还是被挡掉 —— 修 bug 不能把老问题放回来
//   4) 停车 → 起步 能不能恢复
//   5) 漏一个脉冲 中位数能不能扛住
//   6) 停转 → 车速归零

#include "hall_speed.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <functional>

static int g_fail = 0;
static int g_pass = 0;

static void check(const std::string& name, bool cond, const std::string& detail = "") {
    if (cond) {
        g_pass++;
        printf("  ✓ %s\n", name.c_str());
    } else {
        g_fail++;
        printf("  ✗ %s  %s\n", name.c_str(), detail.c_str());
    }
}

static void checkNear(const std::string& name, float got, float want, float tol) {
    bool ok = fabsf(got - want) <= tol;
    char buf[160];
    snprintf(buf, sizeof(buf), "(实测 %.1f, 期望 %.1f ±%.1f)", got, want, tol);
    check(name, ok, buf);
}

// ---------------------------------------------------------------------------
// 1.5.2 的老逻辑 (照抄固件): 参照量 = 最近一次被接受的间隔, 门槛没有上限
// ---------------------------------------------------------------------------
struct OldHall {
    // true = 1.5.1/1.5.2 (车上跑的): 参照量=最近一次, 门槛没上限,
    //        而且**没有参照量时门槛只剩 25ms 下限** (起步那一下的坑)
    // false = 1.5.3: 参照量=中位数, 门槛有上限, 没参照量时用保守的起步值
    bool newLogic = false;
    uint32_t last_us = 0, prev_us = 0;
    uint32_t used_us = 0;
    uint32_t ref_us = 0;             // 最近一次可信间隔
    uint32_t hist[HALL_HISTORY] = {0};
    uint8_t cnt = 0, idx = 0;
    uint32_t glitch = 0;

    bool capped = false;            // 打开就是"新逻辑" (加上限)
    uint32_t gate() const {
        if (newLogic) {
            // 1.5.3+: 没参照量 → 起步值 (保守); 有参照量 → 中位数×60%, 有上限
            if (ref_us == 0) return HALL_GATE_STARTUP_US;
            uint32_t m = HALL_MIN_INTERVAL_US;
            uint32_t a = ref_us * 3 / 5;
            if (a > m) m = a;
            if (m > HALL_GATE_MAX_US) m = HALL_GATE_MAX_US;
            return m;
        }
        // ⚠️ 老逻辑必须用**它自己那份**常量, 不能跟着新常量走 ——
        //    否则改了新参数, 对照组就不是 1.5.2 了, 复现不出来.
        //    1.5.1/1.5.2 的地板就是 25ms, 没有上限, 没有起步值.
        //    1.5.3 加的上限是 80ms —— 对照组必须用 80ms, 不能跟着新常量走.
        const uint32_t OLD_FLOOR_US = 25000;
        const uint32_t OLD_MAX_US = 80000;
        uint32_t min_ok = OLD_FLOOR_US;
        if (ref_us > 0) {
            uint32_t adaptive = ref_us * 3 / 5;
            if (adaptive > min_ok) min_ok = adaptive;
        }
        if (capped && min_ok > OLD_MAX_US) min_ok = OLD_MAX_US;
        return min_ok;               // ← 不 capped 就是 1.5.2 的老逻辑
    }
    uint32_t median() const {
        if (cnt == 0) return 0;
        uint32_t t[HALL_HISTORY];
        for (uint8_t i = 0; i < cnt; i++) t[i] = hist[i];
        for (uint8_t i = 1; i < cnt; i++) {
            uint32_t k = t[i];
            int8_t j = (int8_t)i - 1;
            while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
            t[j + 1] = k;
        }
        return t[cnt / 2];
    }
    void onPulse(uint32_t now) {
        if (now - last_us <= HALL_DEBOUNCE_US) return;
        uint32_t min_ok = gate();
        if (last_us != 0 && now - last_us < min_ok) { glitch++; return; }
        prev_us = last_us;
        last_us = now;
    }
    bool poll(uint32_t now) {
        if (last_us == used_us || prev_us == 0 || last_us <= prev_us) return false;
        used_us = last_us;
        uint32_t iv = last_us - prev_us;
        if (iv >= HALL_STOP_TIMEOUT_US) { cnt = 0; idx = 0; ref_us = 0; return false; }
        uint32_t min_ok = gate();
        if (iv < min_ok) { glitch++; return false; }
        hist[idx] = iv;
        idx = (uint8_t)((idx + 1) % HALL_HISTORY);
        if (cnt < HALL_HISTORY) cnt++;
        ref_us = iv;                 // ← 老逻辑: 参照量 = 最近一次
        return true;
    }
    float kmh(uint32_t = 0) const {
        uint32_t b = median();
        if (b == 0) return 0.0f;
        float period_s = b / 1000000.0f;
        float rpm = 60.0f / (period_s * HALL_MAGNET_COUNT);
        return rpm * HALL_WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;
    }
};

// ---------------------------------------------------------------------------
// 仿真: 一辆车按给定速度曲线跑, 可选地带毛刺
// ---------------------------------------------------------------------------
struct Sim {
    uint32_t t = 0;          // 仿真时钟 (us)
    uint32_t next_pulse = 0; // 下一次真脉冲的时刻
    bool moving = true;      // 轮子是否在转

    // 速度 → 脉冲间隔. 1 磁钢, 轮周长 1.28m.
    static uint32_t intervalFor(float kmh) {
        // I = C * v / kmh  (秒) → 微秒
        return (uint32_t)(HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / kmh * 1000000.0f);
    }

    // 推进 dt 微秒, 期间按 speedAt(t) 生成脉冲.
    // ring = 每个真脉冲后再补三个毛刺 (+3/+6/+55ms), 模拟传感器振铃
    //        (车上实测的偏移范围是 3~60ms, 见 NOTES.md).
    // 每 100ms 调一次 poll —— 固件主循环就是 100ms 一拍 (updateWheelSpeed).
    template <typename H>
    void advance(H& h, uint32_t dt, float (*speedAt)(uint32_t), bool ring,
                 std::function<bool(uint32_t)> skipPulse = nullptr) {
        uint32_t end = t + dt;
        uint32_t next_poll = t + 100000;
        while (t < end) {
            float kmh = speedAt(t);
            if (kmh < 0.5f) {
                // 停转: 轮子不动, 没有脉冲. next_pulse 推远, 等重新起步.
                moving = false;
                next_pulse = t + 1000000;
            } else {
                if (!moving) {
                    moving = true;
                    next_pulse = t;   // 起步: 轮子一动就有脉冲
                }
                uint32_t iv = intervalFor(kmh);
                if (t >= next_pulse) {
                    if (!skipPulse || !skipPulse(next_pulse)) {
                        h.onPulse(next_pulse);
                        if (ring) {
                            // 车上实测的振铃偏移 3~60ms. 30ms 那个最毒:
                            // 1.28m/0.030s = 153km/h (用户说的"一百多")
                            h.onPulse(next_pulse + 3000);
                            h.onPulse(next_pulse + 6000);
                            h.onPulse(next_pulse + 30000);
                        }
                    }
                    next_pulse += iv;
                }
            }
            t += 1000;   // 1ms 一格
            if (t >= next_poll) {
                h.poll(t);
                next_poll = t + 100000;
            }
        }
        t = end;
    }
};

// 速度曲线
static float speedSteady20(uint32_t) { return 20.0f; }
static float speedSteady196(uint32_t) { return 19.6f; }
static float speedSteady36(uint32_t) { return 36.0f; }   // 这台车的最高速
static float speedStopped(uint32_t) { return 0.0f; }

// 19.6 → 40 km/h, 8 秒线性加速, 然后保持
static float speedRamp(uint32_t t) {
    static const uint32_t T0 = 2000000, RAMP = 8000000;
    if (t < T0) return 19.6f;
    if (t < T0 + RAMP) {
        float f = (float)(t - T0) / (float)RAMP;
        return 19.6f + f * (40.0f - 19.6f);
    }
    return 40.0f;
}

// 突然加速: 0.5 秒内从 19.6 冲到 40 (最狠的情况)
static float speedRampFast(uint32_t t) {
    static const uint32_t T0 = 2000000, RAMP = 500000;
    if (t < T0) return 19.6f;
    if (t < T0 + RAMP) {
        float f = (float)(t - T0) / (float)RAMP;
        return 19.6f + f * (40.0f - 19.6f);
    }
    return 40.0f;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 忠实复刻固件时序的仿真: ISR 每次脉冲都跑, 主循环每 100ms 调一次 poll().
// (主循环 100ms 一拍是固件真实行为 —— updateWheelSpeed 里 dt<100 就 return)
// ---------------------------------------------------------------------------
static uint32_t rs = 1;

// 真车日志 (docs/niu485-20261111-090152.log) 里 485 总线相邻帧的间隔 (ms).
// 停车时固件报的假车速周期 ~71ms, 跟这个帧率 (14.1 帧/秒 = 71ms/帧) 几乎一样,
// 所以"总线活动耦合进霍尔线"是个值得上车间证的假设 —— 这里拿真间隔回放.
static const uint32_t BUS_GAP_MS[] = {
    29, 103, 11, 44, 155, 99, 11, 36, 153, 25, 81, 11, 56, 167, 101, 11,
};
static const uint32_t BUS_GAP_N = sizeof(BUS_GAP_MS) / sizeof(BUS_GAP_MS[0]);

struct Ride {
    HallSpeed h;
    uint32_t t = 1000000;
    uint32_t next = 1000000;   // 下一次真脉冲的时刻 (跟 t 同一起点)
    uint32_t lastPoll = 1000000;
    bool moving = false;
    uint32_t emiNext = 0;      // 下一次噪声脉冲
    uint32_t busIdx = 0;
    uint32_t lastTs = 0;       // 上一次注入的时间戳 (保证时间戳单调不减)

    // 跑 dt 微秒.
    //   speedAt      给真实车速
    //   ringMs       每个真沿后补一串振铃 (车上实测偏移 3~60ms); 0 = 不补
    //   emiLo/emiHi  连续噪声的间隔范围 (**毫秒**); 0 = 不加
    //   busNoise     true = 用真车总线间隔回放噪声 (代替 emiLo/emiHi)
    void run(uint32_t dt, float (*speedAt)(uint32_t), uint32_t ringMs,
             uint32_t emiLo, uint32_t emiHi, bool busNoise, uint32_t seed) {
        uint32_t end = t + dt;
        rs = seed * 1103515245u + 12345u;
        while (t < end) {
            float kmh = speedAt(t);
            if (kmh < 0.5f) {
                moving = false;
                next = t + 1000000;
            } else {
                if (!moving) { moving = true; next = t; }
                uint32_t iv = (uint32_t)(1.28f * 3.6f / kmh * 1e6f);
                if (t >= next) {
                    h.onPulse(next);
                    lastTs = next;
                    if (ringMs) {
                        // 车上实测的振铃: 离真沿 3~60ms
                        h.onPulse(next + 3000);
                        h.onPulse(next + 6000);
                        h.onPulse(next + 30000);
                        h.onPulse(next + ringMs * 1000);
                    }
                    next += iv;
                }
            }
            // 噪声 (跟轮子无关): 充电器开关噪声 / 485 总线活动耦合到霍尔线上.
            //
            // ⚠️ **停着的时候也必须注入** —— 之前的版本把噪声塞在"轮子在转"的
            //    分支里, 于是"停车"场景一个噪声脉冲都没有, 测了个寂寞
            //    (老场景 10 永远是 0.0 km/h, 什么逻辑都能通过).
            //    这个坑让"停车报 61km/h"这个 bug 既没被测出来, 也没被测出来.
            //
            // ⚠️ 时间戳必须单调不减: 假脉冲要是落在"真沿 + 振铃"那一组里,
            //    注入顺序就会跟时间顺序对不上 (踩过: 乱序注入导致一个脉冲都没
            //    被拒, 中位数是 0, 看起来像"全被挡了").
            if (busNoise) {
                if (!emiNext) emiNext = t;
                if (t >= emiNext) {
                    uint32_t ts = emiNext < lastTs ? lastTs : emiNext;
                    h.onPulse(ts);
                    lastTs = ts;
                    emiNext += BUS_GAP_MS[busIdx % BUS_GAP_N] * 1000;
                    busIdx++;
                }
            } else if (emiHi) {
                if (!emiNext) emiNext = t;
                if (t >= emiNext) {
                    uint32_t ts = emiNext < lastTs ? lastTs : emiNext;
                    h.onPulse(ts);
                    lastTs = ts;
                    rs = rs * 1103515245u + 12345u;
                    emiNext += (emiLo + (rs >> 16) % (emiHi - emiLo + 1)) * 1000;
                }
            }
            t += 1000;
            if (t - lastPoll >= 100000) { lastPoll = t; h.poll(t); }
        }
    }
};

int main() {
    printf("\n=== 1. 复现 1.5.2 的 bug (老逻辑: 参照量=最近一次, 门槛无上限) ===\n");
    {
        OldHall h;
        Sim sim;
        // 先匀速 20km/h 跑 6 秒, 建立参照量
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        float before = h.kmh(sim.t);
        // 再加速到 40km/h (8 秒), 然后保持 4 秒
        sim.advance(h, 10000000, speedRampFast, false);
        h.poll(sim.t);
        float after = h.kmh(sim.t);
        printf("    加速前 %.1f km/h → 加速后 (真实 40) 显示 %.1f km/h  [毛刺计数 %u]\n",
               before, after, h.glitch);
        check("老逻辑确实会锁死 (显示明显低于真实速度)", after < 25.0f,
              "如果没锁死, 说明这个 bug 已经不存在了, 后面对新逻辑的断言要重写");
        checkNear("老逻辑锁死在真实值的一半 ≈ 19.6km/h", after, 19.6f, 2.0f);
    }

    printf("\n=== 1b. 同一个老逻辑, 只加一条'门槛上限'就能救回来 ===\n");
    {
        OldHall h;
        h.capped = true;    // 只加上限, 其它逻辑不动
        Sim sim;
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        sim.advance(h, 10000000, speedRampFast, false);
        h.poll(sim.t);
        float after = h.kmh(sim.t);
        printf("    老逻辑 + 上限: 加速后 (真实 40) 显示 %.1f km/h\n", after);
        checkNear("只加上限就够, 不用改别的", after, 40.0f, 2.5f);
    }

    printf("\n=== 2. 新逻辑: 同样的加速过程能跟上去 ===\n");
    {
        HallSpeed h;
        Sim sim;
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        float before = h.kmh(sim.t);
        sim.advance(h, 10000000, speedRampFast, false);
        h.poll(sim.t);
        float after = h.kmh(sim.t);
        printf("    加速前 %.1f km/h → 加速后 (真实 40) 显示 %.1f km/h\n", before, after);
        checkNear("加速前车速正确", before, 20.0f, 1.0f);
        checkNear("加速后能跟上真实速度", after, 40.0f, 2.5f);
    }

    printf("\n=== 2b. 突然加速 (0.5 秒内 19.6 → 40) 也不能锁死 ===\n");
    {
        HallSpeed h;
        Sim sim;
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        sim.advance(h, 10000000, speedRampFast, false);
        h.poll(sim.t);
        float after = h.kmh(sim.t);
        printf("    突然加速后 (真实 40) 显示 %.1f km/h\n", after);
        checkNear("突然加速也能跟上", after, 40.0f, 3.0f);
    }

    printf("\n=== 2c. 慢慢加速 (8 秒线性 19.6 → 40) 也能跟上 ===\n");
    {
        HallSpeed h;
        Sim sim;
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        sim.advance(h, 12000000, speedRamp, false);
        h.poll(sim.t);
        float after = h.kmh(sim.t);
        printf("    慢慢加速后 (真实 40) 显示 %.1f km/h\n", after);
        checkNear("慢慢加速也能跟上", after, 40.0f, 3.0f);
    }

    printf("\n=== 3. 毛刺 (振铃) 还是被挡掉 (不能把 1.5.1 修好的问题放回来) ===\n");
    {
        HallSpeed h;
        Sim sim;
        sim.advance(h, 8000000, speedSteady20, true);   // 每个真脉冲后跟 2 个毛刺
        h.poll(sim.t);
        float kmh = h.kmh(sim.t);
        printf("    带毛刺跑 8 秒: 显示 %.1f km/h (真实 20), 毛刺计数 %u\n", kmh, h.glitch_count);
        checkNear("带毛刺时车速仍然是 20", kmh, 20.0f, 1.5f);
        check("毛刺确实被识别了 (计数 > 0)", h.glitch_count > 0);
    }

    printf("\n=== 4. 停车 → 起步 能恢复 ===\n");
    {
        HallSpeed h;
        Sim sim;
        sim.advance(h, 5000000, speedSteady196, false);
        h.poll(sim.t);
        // 停 5 秒 (超过 3s 停转阈值)
        sim.advance(h, 5000000, [](uint32_t) { return 0.0f; }, false);
        h.poll(sim.t);
        float stopped = h.kmh(sim.t);
        // 再起步
        sim.advance(h, 6000000, speedSteady196, false);
        h.poll(sim.t);
        float restarted = h.kmh(sim.t);
        printf("    停 %.1f km/h → 起步后 %.1f km/h\n", stopped, restarted);
        checkNear("停转后车速归零", stopped, 0.0f, 0.01f);
        checkNear("起步后速度恢复", restarted, 19.6f, 1.5f);
    }

    printf("\n=== 5. 漏一个脉冲 中位数扛得住 ===\n");
    {
        HallSpeed h;
        Sim sim;
        uint32_t skipped = 0;
        // 每 7 个脉冲漏 1 个
        sim.advance(h, 8000000, speedSteady20, false, [&](uint32_t p) {
            skipped++;
            return (skipped % 7 == 0);
        });
        h.poll(sim.t);
        float kmh = h.kmh(sim.t);
        printf("    每 7 个漏 1 个: 显示 %.1f km/h (真实 20)\n", kmh);
        checkNear("漏脉冲时车速基本不变", kmh, 20.0f, 2.5f);
    }

    printf("\n=== 6. 闸门上限: 参照量再大也不能超过 HALL_GATE_MAX_US ===\n");
    {
        HallSpeed h;
        h.ref_us = 1000000;   // 假参照量: 1 秒
        uint32_t g = h.gateUs();
        check("门槛被上限挡住", g <= HALL_GATE_MAX_US,
              "门槛无限大就会开始丢真脉冲 → 车速锁死");
        check("门槛不低于绝对下限", g >= HALL_MIN_INTERVAL_US);
        printf("    参照量 1000ms → 门槛 %u us (上限 %u us)\n", g, (unsigned)HALL_GATE_MAX_US);
    }

    printf("\n=== 7. 36km/h 的真间隔必须高于门槛 (否则还是会丢真脉冲) ===\n");
    {
        // 这台车最高 36km/h -> 真间隔 = 1.28 * 3.6 / 36 = 128ms.
        float iv36 = 1.28f * 3.6f / 36.0f * 1000000.0f;
        printf("    36km/h 真间隔 = %.0f us, 门槛上限 = %u us\n", iv36, (unsigned)HALL_GATE_MAX_US);
        check("36km/h 的真脉冲不会被门槛挡住", iv36 > (float)HALL_GATE_MAX_US);
        // 顺带看一眼"要是车比标称的快"还撑不撑得住 (下坡能到 40km/h)
        float iv40 = 1.28f * 3.6f / 40.0f * 1000000.0f;
        check("就算真到 40km/h 也撑得住", iv40 > (float)HALL_GATE_MAX_US);
    }

    printf("\n=== 9. 刚起步 / 停车后再起步: 车速不能飙到 100+ ===\n");
    {
        // 复现: 参照量 = 0 时门槛只剩 25ms 下限 → 26~60ms 的振铃被当成真脉冲
        // → 进了历史 → 中位数被污染 → 车速 = 1.28m/0.030s = 153km/h (十倍!)
        // 老逻辑 (车上跑的 1.5.1/1.5.2) 必然飙, 新逻辑必须不飙.
        auto startupPeak = [](auto& h, Sim& sim) {
            float mx = 0;
            uint32_t t0 = sim.t;
            while (sim.t < t0 + 1000000) {
                sim.advance(h, 20000, speedSteady196, true);
                h.poll(sim.t);
                float k = h.kmh(sim.t);
                if (k > mx) mx = k;
            }
            return mx;
        };

        OldHall old;
        Sim simOld;
        simOld.advance(old, 4000000, speedStopped, false);   // 先停 4 秒
        old.poll(simOld.t);
        float oldPeak = startupPeak(old, simOld);
        printf("    老逻辑 (1.5.1/1.5.2): 起步 1 秒内最高显示 %.1f km/h (真实 19.6)\n", oldPeak);
        check("老逻辑确实会飙到 100+ (复现 bug)", oldPeak > 100.0f,
              "如果没飙, 说明这个 bug 不存在, 下面的断言要重写");

        HallSpeed hn;
        Sim simNew;
        simNew.advance(hn, 4000000, speedStopped, false);
        hn.poll(simNew.t);
        float newPeak = startupPeak(hn, simNew);
        printf("    新逻辑:                起步 1 秒内最高显示 %.1f km/h (真实 19.6)\n", newPeak);
        check("新逻辑起步不飙", newPeak < 30.0f, "起步时没有参照量, 门槛必须保守");
        checkNear("起步 1 秒后车速是对的", hn.kmh(simNew.t), 19.6f, 2.0f);
    }

    printf("\n=== 10. 停车 + 噪声: 假车速必须被压在设计上限以内 ===\n");
    {
        // 实测 (2026-11-11 真车日志): 车停着充电 (电压一路在涨), 固件却连续
        // 11.5 秒报协议值 127 (= 61km/h 封顶 -> 内部间隔 <= 75.5ms), 然后才归零.
        //
        // 100ms 地板把假读数**结构性地**压在 46km/h 以内 (被接受的间隔 >= 100ms
        // -> 车速 <= 1.28*3.6/0.1 = 46.1km/h), 但**压不到 0**:
        // 停着的车和一个真在转的轮子, 间隔落在同一个区间 (100~200ms),
        // 光看间隔软件分不出来. 详见 src/hall_speed.h 的"已知没解决的问题".
        //
        // ⚠️ 老版本的这个场景是**空跑**: 噪声注入写在"轮子在转"的分支里,
        //    停车时一个噪声脉冲都没有 -> 报出 0.0 -> 什么逻辑都能通过.
        Ride r;
        r.run(20000000, speedStopped, 0, 25, 60, false, 7);
        float kmh = r.h.kmh(r.t);
        float vmax = HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / (HALL_MIN_INTERVAL_US / 1000000.0f);
        printf("    停车 + 随机噪声 25~60ms: 报出 %.1f km/h (设计上限 %.1f)\n", kmh, vmax);
        check("停车时的假车速不超过设计上限", kmh <= vmax + 0.5f,
              "被接受的间隔必须 >= 地板, 否则车速上限就失效了");
    }

    printf("\n=== 11. 骑车 + 噪声: 车速不能飙到十倍 ===\n");
    {
        // 用户原话"刚骑车是一百多的速度, 好像是十倍".
        // 25ms 地板时, 一个 30ms 的振铃就能进历史 -> 1.28/0.030 = 153km/h.
        Ride r;
        r.run(30000000, speedSteady196, 0, 25, 60, false, 9);
        float kmh = r.h.kmh(r.t);
        printf("    骑车 19.6km/h + 随机噪声 25~60ms: 报出 %.1f km/h\n", kmh);
        check("EMI 干扰下车速不会飙到十倍", kmh < 60.0f,
              "刚骑车飙到 100+km/h (十倍) 就是这么来的");
    }

    printf("\n=== 12. 36km/h (这车的最高速) + 振铃: 车速必须还是 36 ===\n");
    {
        // ⚠️ 这是"判据用物理脉冲"那版会翻车的地方:
        //   36km/h 的真间隔是 128ms, 而振铃最后一个沿在真沿后 60ms,
        //   于是下一个真沿离上一个振铃沿只有 128-60 = 68ms —— 门槛 70ms 时
        //   **真沿被当成毛刺丢掉**, 这一拍量到 2 倍间隔, 车速显示一半.
        //   越骑越快丢得越多, 高速时整段读数是 0.
        Ride r;
        r.run(20000000, speedSteady36, 60, 0, 0, false, 3);
        float kmh = r.h.kmh(r.t);
        printf("    36km/h + 振铃(离真沿 3~60ms): 报出 %.1f km/h (真实 36)\n", kmh);
        checkNear("36km/h + 振铃时车速正确", kmh, 36.0f, 2.0f);
    }

    printf("\n=== 13. 36km/h + 真车总线噪声: 车速必须还是 36 ===\n");
    {
        // 用真车日志里的真实 485 帧间隔当噪声 (14 帧/秒, 间隔 11~178ms)
        Ride r;
        r.run(20000000, speedSteady36, 0, 0, 0, true, 5);
        float kmh = r.h.kmh(r.t);
        printf("    36km/h + 总线噪声(真间隔回放): 报出 %.1f km/h (真实 36)\n", kmh);
        checkNear("总线噪声下车速正确", kmh, 36.0f, 4.0f);
    }

    printf("\n=== 14. 36km/h + 振铃 + 总线噪声 (最恶劣): 不能飙到设计上限以外 ===\n");
    {
        // 这个组合现实里大概不会同时出现 (车上骑起来时充电器是关的),
        // 留着是为了守住"最坏情况下也不能离谱"这条底线.
        Ride r;
        r.run(20000000, speedSteady36, 60, 0, 0, true, 11);
        float kmh = r.h.kmh(r.t);
        float vmax = HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / (HALL_MIN_INTERVAL_US / 1000000.0f);
        printf("    36km/h + 振铃 + 总线噪声: 报出 %.1f km/h (真实 36, 设计上限 %.1f)\n", kmh, vmax);
        check("最恶劣组合下也不超过设计上限", kmh <= vmax + 0.5f,
              "被接受的间隔必须 >= 地板, 否则车速上限就失效了");
        printf("    ⚠️ 噪声脉冲和真脉冲混在一起时读数会偏高 (~20%%): "
               "一个磁钢 + 一根长线就是这么个极限, 根治要靠硬件滤波\n");
    }

    printf("\n=== 8. 设计边界: 门槛对应的最高车速 (46.1km/h) ===\n");
    {
        // 地板 100ms -> 真间隔不能短于 100ms -> 1.28m 轮子最高
        // 1.28*3.6/0.100 = 46.1km/h. 这台车最高 36km/h, 余量 28% ✓
        float v_max = HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / (HALL_GATE_MAX_US / 1000000.0f);
        printf("    门槛上限 %.0f us → 最高可测车速 %.1f km/h\n",
               (double)HALL_GATE_MAX_US, v_max);
        check("设计上限高于这车实际的最高车速 (36km/h)", v_max > 36.0f);
        check("设计上限远高于实测振铃偏移 (60ms)",
              HALL_GATE_MAX_US > 60000);
    }

    printf("\n%d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
