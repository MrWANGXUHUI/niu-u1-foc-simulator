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
    uint32_t last_us = 0, prev_us = 0;
    uint32_t used_us = 0;
    uint32_t ref_us = 0;             // 最近一次可信间隔
    uint32_t hist[HALL_HISTORY] = {0};
    uint8_t cnt = 0, idx = 0;
    uint32_t glitch = 0;

    bool capped = false;            // 打开就是"新逻辑" (加上限)
    uint32_t gate() const {
        uint32_t min_ok = HALL_MIN_INTERVAL_US;
        if (ref_us > 0) {
            uint32_t adaptive = ref_us * 3 / 5;
            if (adaptive > min_ok) min_ok = adaptive;
        }
        if (capped && min_ok > HALL_GATE_MAX_US) min_ok = HALL_GATE_MAX_US;
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
    bool started = false;

    // 速度 → 脉冲间隔. 1 磁钢, 轮周长 1.28m.
    static uint32_t intervalFor(float kmh) {
        // I = C * v / kmh  (秒) → 微秒
        return (uint32_t)(HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / kmh * 1000000.0f);
    }

    // 推进 dt 微秒, 期间按 speedAt(t) 生成脉冲.
    // ring = 每个真脉冲后再补两个毛刺 (+3ms / +6ms), 模拟传感器振铃.
    // 每 100ms 调一次 poll —— 固件主循环就是 100ms 一拍 (updateWheelSpeed).
    template <typename H>
    void advance(H& h, uint32_t dt, float (*speedAt)(uint32_t), bool ring,
                 std::function<bool(uint32_t)> skipPulse = nullptr) {
        uint32_t end = t + dt;
        uint32_t next_poll = t + 100000;
        bool moving = true;
        while (t < end) {
            float kmh = speedAt(t);
            if (kmh < 0.5f) {
                // 停转: 不产生脉冲 (车轮不动)
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
                            h.onPulse(next_pulse + 3000);
                            h.onPulse(next_pulse + 6000);
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

    printf("\n=== 7. 40km/h 的真间隔必须高于门槛上限 (否则还是会丢真脉冲) ===\n");
    {
        float iv40 = 1.28f * 3.6f / 40.0f * 1000000.0f;
        printf("    40km/h 真间隔 = %.0f us, 门槛上限 = %u us\n", iv40, (unsigned)HALL_GATE_MAX_US);
        check("40km/h 的真脉冲不会被上限挡住", iv40 > (float)HALL_GATE_MAX_US);
    }

    printf("\n=== 8. 设计边界: 门槛上限对应的最高车速 (57.6km/h) ===\n");
    {
        // 上限 80ms → 真间隔不能短于 80ms → 1.28m 轮子最高 1.28*3.6/0.080 = 57.6km/h.
        // 这台车最高 40km/h (真间隔 115ms), 离边界还远 ✓
        float v_max = HALL_WHEEL_CIRCUMFERENCE_M * 3.6f / (HALL_GATE_MAX_US / 1000000.0f);
        printf("    门槛上限 %.0f us → 最高可测车速 %.1f km/h\n",
               (double)HALL_GATE_MAX_US, v_max);
        check("设计上限高于这车实际的最高车速 (40km/h)", v_max > 40.0f);
        check("设计上限远高于实测振铃偏移 (60ms)",
              HALL_GATE_MAX_US > 60000);
    }

    printf("\n%d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
