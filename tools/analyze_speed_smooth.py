#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
车速平滑度分析 —— 从 485 总线日志里把车速序列抽出来, 判断"平不平顺".

用法:
    python3 tools/analyze_speed_smooth.py docs/niu485-20261111-090152.log
    python3 tools/analyze_speed_smooth.py docs/*.log          # 一次看多个

车速不是直接测的, 是固件算出来的:
    霍尔脉冲间隔 (取中位数) → 车速 km/h → 协议值 round(kmh×25/12), 封顶 127
    → 放进我们回给 V35 的 0x82 应答的 data[9] → V35 换算成仪表值 (×12/25)

所以日志里只要有 TX 的 0x82 帧, 就能还原出**每一拍我们报出去的车速**.
两个换算系数都来自 NOTES.md §3, 别改.

    协议值 → 仪表显示 km/h : 值 × 12/25
    协议值 → 固件真实 km/h : 值 × 25/12   (仪表那套是 ECU 换算过的, 会偏小)

⚠️ 光看"曲线平不平"不够 —— 一条死平的线看起来很"平滑", 但那是**根本没在变**,
   是测速断了, 不是车开得稳. 所以这个脚本再把 BMS 电流拉出来对照车速.

⚠️⚠️ **BMS 电流不能当"车在不在动"的地面真相** —— 这是踏板车, 纯人力骑行/滑行时
   电流就是 0.1~0.2A, 跟停着一模一样 (2026-11-16 用户纠正过一次这个误判).
   所以"电流低 + 车速高"只是**嫌疑**, 不是定论; 要判真假得问人, 或者看串口日志里
   霍尔物理间隔的规律性 (71ms 规整 = 485 总线, 25~60ms 随机 = 充电器).

输出四段:
    1. 车速序列     —— 采样率、范围、走势 (火花图)
    2. 平滑度       —— 恒定段 / 阶跃 / 抖动, 直接给结论
    3. 电流对照     —— BMS 电流 vs 车速 (**只作参考**, 见上面的警告)
    4. 结论 + 嫌疑  —— 指向是"测速假脉冲"还是"真车速"

配套: tools/analyze_485_log.py (看总线/校验和/应答延迟), 这个专门看车速.
"""

import re
import sys
import os
from statistics import median

# ---- 协议常量 (见 niu_u1_foc_simulator/src/NOTES.md §3 和 bms_protocol.h) ----
FOC_ADDR = (0x20, 0xDF)      # 我们自己
DASH_ADDR = (0x10, 0xEF)     # 仪表
BMS_ADDR = (0x31, 0xCE)      # 电池 BMS
FUNC_REPLY = 0x82            # 功能码最高位 1 = 回话

PROTO_MAX = 127              # 协议车速值封顶 (int8)
KMH_PER_PROTO = 12.0 / 25.0  # 协议值 → 仪表显示 km/h (ECU 的换算)
PROTO_PER_KMH = 25.0 / 12.0  # km/h → 协议值

# 轮子周长 1.28 m, 单磁钢 → 转速 = 60/间隔秒, 车速 = rpm × 1.28 × 60 / 1000
#   = (60/间隔秒) × 1.28 × 60 / 1000 = 4.608 / 间隔秒
# 所以反过来: 间隔秒 = 4.608 / 车速kmh
KMH_FROM_PERIOD = 4.608

# 这车最高 36 km/h (2026-11-16 用户上车确认) → 真脉冲间隔 ≥ 128 ms
# (hall_speed.h 的注释里也是这个数). 比这更密的间隔, 物理上只能是干扰/振铃 ——
# 拿它算出来的车速是假的.
REAL_MIN_PERIOD_MS = 128.0
# 车上实测的干扰节奏: 离真沿 3~60 ms (NOTES §7.4)
NOISE_BAND_MS = (25.0, 60.0)

# BMS 电流字节要除以这个才是安培 (见 bms_protocol.h 的 BMS_CURRENT_SCALE)
BMS_CURRENT_SCALE = 10.0
BMS_FULL_LEN = 55            # 状态 15 字节 + 单体 40 字节 拼在一帧
BMS_STATUS_LEN = 15

# 电流"低"的阈值 (安培). ⚠️ 这**不能**用来判"车停着" —— 踏板车滑行时也是这个量级,
# 停车 vs 滑行靠电流分不出来. 这里只拿它当"电机有没有在出力"的粗略信号.
IDLE_AMP = 1.0

# 火花图
SPARK = '▁▂▃▄▅▆▇█'


class Frame(object):
    """一帧 485 报文."""

    def __init__(self, direction, t_ms, raw):
        self.dir = direction
        self.t = t_ms
        self.raw = raw

    @property
    def addr(self):
        return (self.raw[1], self.raw[2]) if len(self.raw) > 2 else (0, 0)

    @property
    def func(self):
        return self.raw[4] if len(self.raw) > 4 else 0

    @property
    def data_len(self):
        return self.raw[5] if len(self.raw) > 5 else 0

    @property
    def data(self):
        """数据区 (已 -0x33 反混淆)."""
        n = self.data_len
        if len(self.raw) < 6 + n:
            return []
        return [(b - 0x33) & 0xFF for b in self.raw[6:6 + n]]

    @property
    def checksum_ok(self):
        """校验和 = 从字节 0 累加到 (不含) CS 字节, 取低 8 位 (NOTES §3)."""
        if len(self.raw) < 8 or self.raw[-1] != 0x16:
            return False
        s = 0
        for b in self.raw[:-2]:
            s = (s + b) & 0xFF
        return s == self.raw[-2]


def parse_log(path):
    """读日志 → (frames, 头信息 dict)."""
    frames = []
    head = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#'):
                m = re.match(r'#\s*固件\s*([\d.]+)', line)
                if m:
                    head['fw'] = m.group(1)
                m = re.match(r'#\s*APP 车速\s*([\d.]+)\s*km/h.*协议车速\s*(\d+)', line)
                if m:
                    head['app_speed'] = float(m.group(1))
                    head['app_proto'] = int(m.group(2))
                continue
            m = re.match(r'^(RX|TX)\s+(\d+)\s+(.*)$', line)
            if not m:
                continue
            try:
                raw = [int(x, 16) for x in m.group(3).split()]
            except ValueError:
                continue
            if len(raw) >= 6:
                frames.append(Frame(m.group(1), int(m.group(2)), raw))
    return frames, head


def extract_speed(frames):
    """我们回给 V35 的 0x82 应答 → [(t, 协议车速值)].

    只有 TX 方向、地址对 20 DF、功能码 0x82、长度够 12 字节的才算数
    (data[9] 才是车速字节, 前面少一个字节就错位).
    """
    out = []
    for f in frames:
        if f.dir != 'TX' or f.addr != FOC_ADDR or f.func != FUNC_REPLY:
            continue
        if f.data_len < 12:
            continue
        d = f.data
        if len(d) < 10:
            continue
        out.append((f.t, d[9]))
    return out


def extract_bms(frames):
    """BMS 的 55 字节状态帧 → [(t, 电压V, 电流A, 电量%, 温度列表)]."""
    out = []
    for f in frames:
        if f.dir != 'RX' or f.addr != BMS_ADDR or f.func != FUNC_REPLY:
            continue
        if f.data_len != BMS_FULL_LEN:
            continue
        d = f.data
        if len(d) < BMS_STATUS_LEN:
            continue
        volt = (d[0] << 8 | d[1]) / 10.0
        cur = d[5] / BMS_CURRENT_SCALE
        soc = d[6]
        temps = [t - 256 if t > 127 else t for t in d[10:15]]
        out.append((f.t, volt, cur, soc, temps))
    return out


def extract_dash(frames):
    """V35 发给仪表的 0x04 帧 → [(t, 15字节)]. 用来对账仪表看到的数."""
    out = []
    for f in frames:
        if f.dir == 'RX' and f.addr == DASH_ADDR and f.func == 0x04 and f.data_len == 15:
            out.append((f.t, f.data))
    return out


def sparkline(vals, width=60):
    """把一串数画成火花图."""
    if not vals:
        return ''
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return SPARK[len(SPARK) // 2] * min(len(vals), width)
    # 采样点太多就按 width 均匀抽稀 (每格取平均)
    if len(vals) > width:
        step = len(vals) / float(width)
        vals = [sum(vals[int(i * step):max(int((i + 1) * step), int(i * step) + 1)]) /
                len(vals[int(i * step):max(int((i + 1) * step), int(i * step) + 1)])
                for i in range(width)]
    out = []
    for v in vals:
        idx = int((v - lo) / (hi - lo) * (len(SPARK) - 1) + 0.5)
        out.append(SPARK[idx])
    return ''.join(out)


def analyze_smoothness(series):
    """把车速序列的"平不平"量化出来.

    series: [(t_ms, 协议车速值)]
    返回一个 dict, 里面是所有指标.
    """
    res = {'n': len(series)}
    if not series:
        return res

    ts = [t for t, _ in series]
    vals = [v for _, v in series]
    res['t0'], res['t1'] = ts[0], ts[-1]
    res['span_s'] = (ts[-1] - ts[0]) / 1000.0
    res['min'], res['max'] = min(vals), max(vals)
    res['unique'] = sorted(set(vals))

    # 采样间隔
    dts = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
    if dts:
        res['dt_med'] = median(dts)
        res['dt_min'] = min(dts)
        res['dt_max'] = max(dts)

    # 恒定段: 连续同值的最长一段
    runs = []
    start = 0
    for i in range(1, len(vals) + 1):
        if i == len(vals) or vals[i] != vals[start]:
            runs.append((vals[start], start, i - 1))
            start = i
    res['runs'] = runs
    longest = max(runs, key=lambda r: r[2] - r[1]) if runs else None
    res['longest_run'] = longest
    if longest:
        res['longest_run_s'] = (ts[longest[2]] - ts[longest[1]]) / 1000.0
        res['longest_run_pct'] = 100.0 * (longest[2] - longest[1] + 1) / len(vals)
    # 有多少采样点跟前一点是同一个值 (值没变)
    res['flat_pts'] = sum(1 for i in range(1, len(vals)) if vals[i] == vals[i - 1])
    res['flat_pct'] = 100.0 * res['flat_pts'] / len(vals)

    # 阶跃: 相邻两点值变了
    steps = []
    for i in range(len(vals) - 1):
        if vals[i + 1] != vals[i]:
            dt = ts[i + 1] - ts[i]
            steps.append({
                'i': i, 't': ts[i + 1], 'from': vals[i], 'to': vals[i + 1],
                'dproto': vals[i + 1] - vals[i],
                'dkmh': (vals[i + 1] - vals[i]) * KMH_PER_PROTO,
                'dt': dt,
                'jerk': (vals[i + 1] - vals[i]) * KMH_PER_PROTO / (dt / 1000.0) if dt else 0.0,
            })
    res['steps'] = steps
    res['n_steps'] = len(steps)
    # 抖动 = 只差 1 格 (±0.48 km/h) 的小变化, 是量化噪声
    res['n_jitter'] = sum(1 for s in steps if abs(s['dproto']) == 1)
    res['n_big_step'] = sum(1 for s in steps if abs(s['dproto']) >= 2)
    if steps:
        worst = max(steps, key=lambda s: abs(s['dkmh']))
        res['worst'] = worst
        res['max_dkmh'] = abs(worst['dkmh'])
        res['max_jerk'] = max(abs(s['jerk']) for s in steps)

    # 方向反转次数 (涨-跌-涨 = 抖)
    signs = [1 if s['dproto'] > 0 else (-1 if s['dproto'] < 0 else 0) for s in steps]
    signs = [s for s in signs if s != 0]
    res['reversals'] = sum(1 for i in range(1, len(signs)) if signs[i] != signs[i - 1])

    # 封顶 / 归零
    res['n_max'] = sum(1 for v in vals if v >= PROTO_MAX)
    res['n_zero'] = sum(1 for v in vals if v == 0)
    return res


def cross_check(speed_series, bms_series):
    """车速 vs BMS 电流 —— 车到底动没动.

    每对: 取该时刻之前最近的一拍车速.
    """
    pairs = []
    for t, volt, cur, soc, temps in bms_series:
        sp = None
        for ts, v in speed_series:
            if ts <= t:
                sp = v
            else:
                break
        if sp is None:
            continue
        pairs.append({
            't': t, 'proto': sp, 'kmh': sp * KMH_PER_PROTO,
            'cur': cur, 'abs_cur': abs(cur), 'volt': volt, 'soc': soc, 'temps': temps,
        })
    return pairs


def implied_period_ms(proto):
    """协议车速值 → 固件算车速时用的脉冲中位间隔 (ms).

    协议值 = round(kmh × 25/12), 而 kmh = 4.608 / 间隔秒
    → 间隔ms = 4.608 × 1000 / (proto × 12/25) = 9600 / proto

    协议值封顶 127 时只能反推出**上界** —— 真实间隔比这个还短, 车速比 61 还高.
    """
    if proto <= 0:
        return None
    return 9600.0 / proto


def analyze_bad_frames(frames):
    """校验和不对的帧 —— 是数据区坏了, 还是只有校验和字节坏了?

    这两者的含义完全不同:
      数据区也坏了   → 车速可能是被总线噪声撞出来的假数
      只有 CS 坏了   → 数据区是固件真发的, 值可信 (但 V35 会把这一帧丢掉)
    """
    out = []
    for f in frames:
        if f.checksum_ok:
            continue
        s = 0
        for b in f.raw[:-2]:
            s = (s + b) & 0xFF
        xor = f.raw[-2] ^ s
        # 邻帧 (同类型同长度) 的数据区 —— 拿来比对, 看数据区本身有没有被动过
        neighbours = [g for g in frames
                      if g is not f and g.dir == f.dir and g.addr == f.addr
                      and g.func == f.func and g.data_len == f.data_len
                      and 0 < abs(g.t - f.t) < 2000]
        same_data = any(g.data == f.data for g in neighbours)
        speed_proto = None
        if (f.dir == 'TX' and f.addr == FOC_ADDR and f.func == FUNC_REPLY
                and len(f.data) >= 10):
            speed_proto = f.data[9]
        out.append({
            'frame': f, 'expected_cs': s, 'xor': xor,
            'single_bit': bool(xor) and (xor & (xor - 1)) == 0,
            'data_intact': same_data,
            'speed_proto': speed_proto,
        })
    return out


def verdict(smooth, pairs):
    """给一句话结论."""
    if not smooth.get('n'):
        return '日志里没有我们的状态应答帧 (TX 0x82), 没法看车速.'

    issues = []
    if smooth['n_steps'] == 0:
        issues.append('车速全程一个值都没变 (恒定 %.1f s) —— 这不是"平滑", 是**没在更新**'
                      % smooth.get('longest_run_s', 0))
    else:
        if smooth['n_big_step']:
            issues.append('有 %d 次大跳变 (≥2 格), 最大一跳 %.1f km/h'
                          % (smooth['n_big_step'], smooth['max_dkmh']))
        if smooth['n_jitter']:
            issues.append('有 %d 次 ±1 格的量化抖动' % smooth['n_jitter'])
        if smooth['reversals'] >= 3:
            issues.append('方向反转 %d 次 (上下跳)' % smooth['reversals'])

    # 电流低时车速不为零 —— 只是**嫌疑**, 不是定论 (踏板车滑行电流也低)
    fake = []
    for p in pairs:
        if p['abs_cur'] < IDLE_AMP and p['kmh'] > 5.0:
            fake.append(p)
    if fake:
        issues.append('**电流很低时却报出车速**: %d 个采样点里 |电流| < %.1fA 却报 %.1f km/h '
                      '—— 嫌疑是假速度, 但踏板车滑行电流也这么低, 要跟人确认当时车在不在动'
                      % (len(fake), IDLE_AMP, max(p['kmh'] for p in fake)))

    # 踩着走却报不动
    moving = [p for p in pairs if p['abs_cur'] >= 5.0]
    if moving and max(p['kmh'] for p in moving) < 5.0:
        issues.append('**电机在转却几乎没有车速**: |电流| ≥ 5A 时最高才 %.1f km/h —— 测速断了'
                      % max(p['kmh'] for p in moving))

    if smooth['n_max']:
        issues.append('协议车速顶到封顶值 %d (仪表 %.1f km/h) —— 数值被截断了, 真实值比这更高'
                      % (PROTO_MAX, PROTO_MAX * KMH_PER_PROTO))

    # 反推出来的脉冲间隔短于物理下限 → 这个车速根本不可能是车跑出来的
    for v in smooth['unique']:
        if v <= 0 or v >= PROTO_MAX:
            continue
        p = implied_period_ms(v)
        if p is not None and p < REAL_MIN_PERIOD_MS:
            issues.append('协议值 %d 反推出脉冲间隔 %.0f ms —— 比这车跑满速时的 %.0f ms 还短, '
                          '**物理上不可能是轮子转出来的** (只能是干扰/振铃)'
                          % (v, p, REAL_MIN_PERIOD_MS))
            break

    return issues


def report(path):
    frames, head = parse_log(path)
    print('=' * 78)
    print('日志: %s' % os.path.basename(path))
    print('  固件 %s | 帧数 %d' % (head.get('fw', '?'), len(frames)))
    if frames:
        print('  覆盖时间窗: %.1f s (t=%d..%d, 板子自己的毫秒时钟)'
              % ((frames[-1].t - frames[0].t) / 1000.0, frames[0].t, frames[-1].t))
    if 'app_speed' in head:
        print('  App 侧最后收到: %.1f km/h (协议车速 %d)'
              % (head['app_speed'], head.get('app_proto', -1)))

    # ---- 1. 车速序列 ----
    speed = extract_speed(frames)
    print()
    print('--- 1. 车速序列 (我们回给 V35 的 0x82 帧 data[9]) ---')
    if not speed:
        print('  没有 TX 的 0x82 应答帧 —— 固件没回话, 谈不上车速.')
        return
    kmhs = [v * KMH_PER_PROTO for _, v in speed]
    smooth = analyze_smoothness(speed)
    print('  采样 %d 点, 中位间隔 %.0f ms (最短 %.0f / 最长 %.0f)'
          % (len(speed), smooth.get('dt_med', 0),
             smooth.get('dt_min', 0), smooth.get('dt_max', 0)))
    print('  协议车速值 %d ~ %d  →  仪表显示 %.1f ~ %.1f km/h'
          % (min(v for _, v in speed), max(v for _, v in speed),
             min(kmhs), max(kmhs)))
    print('  仪表值走势 (火花图, 越高越快):')
    print('    ' + sparkline(kmhs))

    # ---- 2. 平滑度 ----
    print()
    print('--- 2. 平滑度 ---')
    print('  取值种类: %d 种 %s' % (len(smooth['unique']),
                                    '' if len(smooth['unique']) <= 8 else '(前 8 种)'))
    if len(smooth['unique']) > 8:
        print('    %s ...' % ' '.join(str(v) for v in smooth['unique'][:8]))
    print('  恒定段 (值不变): 最长 %d 点 = %.1f s (占 %.0f%%)'
          % (smooth['longest_run'][2] - smooth['longest_run'][1] + 1,
             smooth.get('longest_run_s', 0), smooth.get('longest_run_pct', 0)))
    print('  阶跃 %d 次 (其中 ±1 格的小抖动 %d 次, ≥2 格的硬跳 %d 次), 方向反转 %d 次'
          % (smooth['n_steps'], smooth['n_jitter'], smooth['n_big_step'],
             smooth['reversals']))
    if smooth['n_steps']:
        print('  变化明细 (t / 从 → 到 / 变化 / 间隔 / 加加速度):')
        for s in smooth['steps'][:20]:
            print('    t=%-7d %3d → %-3d (%+6.1f km/h)  用时 %4d ms  %+7.1f km/h/s'
                  % (s['t'], s['from'], s['to'], s['dkmh'], s['dt'], s['jerk']))
        if len(smooth['steps']) > 20:
            print('    ... 还有 %d 次' % (len(smooth['steps']) - 20))
    print('  封顶(=%d) %d 点, 归零 %d 点' % (PROTO_MAX, smooth['n_max'], smooth['n_zero']))

    # ---- 2b. 反推脉冲间隔 ----
    # 车速是"脉冲间隔的函数", 所以从车速值能反推固件当时看到的间隔.
    # 这是最有用的一栏: 间隔短于 115ms (=40km/h) 的, 物理上只能是干扰.
    print()
    print('--- 2b. 从车速值反推固件看到的脉冲间隔 ---')
    print('  (车速 km/h = 4.608 / 间隔秒; 真车最高 40km/h → 间隔 ≥ %.0f ms, 更短的都是干扰)'
          % REAL_MIN_PERIOD_MS)
    for v in smooth['unique']:
        cnt = sum(1 for _, x in speed if x == v)
        p = implied_period_ms(v)
        if p is None:
            print('    协议值 %3d ×%2d 点 → 车速 %5.1f km/h → 无脉冲 (停转)'
                  % (v, cnt, v * KMH_PER_PROTO))
            continue
        if v >= PROTO_MAX:
            print('    协议值 %3d ×%2d 点 → 车速 ≥%5.1f km/h → 间隔 ≤%6.1f ms (**封顶, 真实值更高**)'
                  % (v, cnt, v * KMH_PER_PROTO, p))
            continue
        tag = ''
        if p < REAL_MIN_PERIOD_MS:
            tag = '  ⚠️ 短于真车最快时的间隔 → **物理上不可能, 是干扰**'
            if NOISE_BAND_MS[0] <= p <= NOISE_BAND_MS[1]:
                tag += ' (落在实测干扰带 %.0f~%.0f ms 内)' % NOISE_BAND_MS
        print('    协议值 %3d ×%2d 点 → 车速 %5.1f km/h → 间隔 %6.1f ms%s'
              % (v, cnt, v * KMH_PER_PROTO, p, tag))

    # ---- 3. 电流对照 ----
    bms = extract_bms(frames)
    print()
    print('--- 3. 电流对照 (BMS 电流; ⚠️ 踏板车滑行电流也低, 不能据此判"车在不在动") ---')
    if not bms:
        print('  日志里没有 BMS 状态帧, 拿不到电流.')
        pairs = []
    else:
        print('  BMS 状态 %d 帧: 电压 %.1f~%.1f V, 电量 %d%%, 温度 %s°C'
              % (len(bms), min(b[1] for b in bms), max(b[1] for b in bms),
                 bms[0][3], '/'.join(str(t) for t in bms[0][4])))
        print('  电流: %s A' % ' '.join('%.1f' % b[2] for b in bms))
        pairs = cross_check(speed, bms)
        idle = [p for p in pairs if p['abs_cur'] < IDLE_AMP]
        moving = [p for p in pairs if p['abs_cur'] >= 5.0]
        print('  |电流| < %.1fA (电流低, 车可能在滑行) 的 %d 点里, 车速: %s'
              % (IDLE_AMP, len(idle),
                 ' '.join('%.1f' % p['kmh'] for p in idle) if idle else '(无)'))
        if moving:
            print('  |电流| ≥ 5A (电机在转) 的 %d 点里, 车速: %s'
                  % (len(moving), ' '.join('%.1f' % p['kmh'] for p in moving)))
        # 相关性
        if len(pairs) > 2:
            n = len(pairs)
            mx = sum(p['kmh'] for p in pairs) / n
            mi = sum(p['abs_cur'] for p in pairs) / n
            num = sum((p['kmh'] - mx) * (p['abs_cur'] - mi) for p in pairs)
            den = (sum((p['kmh'] - mx) ** 2 for p in pairs) *
                   sum((p['abs_cur'] - mi) ** 2 for p in pairs)) ** 0.5
            r = num / den if den else 0.0
            print('  车速 vs |电流| 相关系数 r = %+.2f  (%s)'
                  % (r, '同向, 车速跟着电流走' if r > 0.5 else
                     ('反向' if r < -0.5 else '基本无关 —— 车速跟车实际动没动没关系')))

    # ---- 3b. 仪表帧对账 ----
    # V35 每 200ms 给仪表发一帧 0x04, 仪表上的车速就是这么来的.
    # 帧里哪几个字节跟着车速变, 就说明 V35 确实把我们的车速值送到了仪表.
    dash = extract_dash(frames)
    if dash:
        print()
        print('--- 3b. 仪表帧 (V35 → 仪表, 0x04) 对账 ---')
        first = dash[0][1]
        changed = [i for i in range(len(first)) if any(g[i] != first[i] for _, g in dash)]
        print('  %d 帧, 15 字节里跟着车速变的是第 %s 字节'
              % (len(dash), changed if changed else '(无)'))
        if changed:
            vals = sorted(set(g[i] for _, g in dash for i in changed))
            print('  这些字节的取值: %s' % ' '.join('0x%02X' % v for v in vals))
        # 变化点跟车速跳变点对不对得上
        segs = []
        cur = dash[0][1]
        start = dash[0][0]
        for t, g in dash[1:]:
            if g != cur:
                segs.append((start, t, cur, g))
                cur = g
                start = t
        if segs:
            for t0, t1, a, b in segs:
                print('  t=%d..%d 仪表帧变化: %s → %s'
                      % (t0, t1, ' '.join('%02X' % x for x in a),
                         ' '.join('%02X' % x for x in b)))
            step_ts = [s['t'] for s in smooth.get('steps', [])]
            for t0, t1, a, b in segs:
                near = [t for t in step_ts if t0 - 600 <= t <= t1 + 600]
                if near:
                    lag = t1 - sum(near) / len(near)
                    print('    ↑ 跟车速跳变 t=%d 对得上 (仪表这边隔了 ~%.0f ms 才跟着变)'
                          % (near[0], lag))
        else:
            print('  整段日志仪表帧没变过 —— 车速从头到尾是一个值.')

    # ---- 4. 结论 ----
    print()
    print('--- 4. 结论 ---')
    issues = verdict(smooth, pairs)
    if not issues:
        print('  车速序列连续变化, 没有阶跃也没有假速度 —— 平滑 ✓')
    else:
        for i in issues:
            print('  • %s' % i)

    # ---- 4.5 坏帧 ----
    bad = analyze_bad_frames(frames)
    print()
    print('--- 4.5 校验和 (区分"值被撞坏"和"只有 CS 被撞坏") ---')
    if not bad:
        print('  %d/%d 帧全部校验和正确 —— 总线干净, 车速跳变是固件自己算出来的, 不是噪声撞的'
              % (len(frames), len(frames)))
    for b in bad:
        f = b['frame']
        tag = '单 bit 翻转' if b['single_bit'] else '多 bit (XOR=%02X)' % b['xor']
        print('  %s t=%d: 实际 CS=%02X, 应为 %02X (%s)'
              % (f.dir, f.t, f.raw[-2], b['expected_cs'], tag))
        if b['data_intact']:
            print('     数据区跟邻帧**完全一致** → 值是真的, 只有校验和字节被撞了'
                  ' (V35 会把这一帧丢掉, 但不是它把车速弄成假的)')
        else:
            print('     数据区跟邻帧对不上 → 这一帧的内容本身不可信')
        if b['speed_proto'] is not None:
            print('     该帧车速字节 = 协议值 %d' % b['speed_proto'])
    # 跳变点是不是正好落在坏帧上
    step_ts = [s['t'] for s in smooth.get('steps', [])]
    bad_ts = [b['frame'].t for b in bad]
    collide = [t for t in step_ts if t in bad_ts]
    if collide:
        print('  ⚠️ 车速跳变点 %s 正好落在坏帧上 —— 但看上面的数据区比对, 值仍然可信'
              % ', '.join('t=%d' % t for t in collide))
    print()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        print('错误: 没有给日志文件')
        return 1
    for p in args:
        if not os.path.exists(p):
            print('找不到文件: %s' % p)
            continue
        report(p)
    return 0


if __name__ == '__main__':
    sys.exit(main())
