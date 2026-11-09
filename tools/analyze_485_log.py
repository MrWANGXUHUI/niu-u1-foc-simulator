#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小牛 U1 485 总线日志 —— 离线分析脚本

用法:
    python3 tools/analyze_485_log.py niu485-20261104-213012.log

日志是小程序里点「保存 485 日志」拉回来、发给微信再存到电脑上的那个文本文件.
每行一帧:

    RX 12345 68 40 BF 68 02 02 05 08
    TX 12358 68 20 DF 68 82 0C 63 4C ...

    RX = 总线上收到的   TX = 我们 (FOC 模拟器) 自己发出去的
    第一个数 = 毫秒时间戳 (板子自己的时钟, 从开机算起)
    后面 = 这一帧的原始字节 (大写十六进制)

这个脚本回答四个问题:
    1) 总线通不通, 快不快
    2) V35 轮询我们之后, 我们应答得及不及时 (V35 单次超时只有 ~20ms,
       超了 ECU 就置错误码 190, 仪表车速显示 0 —— 这是"有时候没有速度"的根因)
    3) BMS 有没有回话, 回的话数据对不对 (电压/电量/单体)
    4) 有没有校验和错、长度不符、没见过的功能码 (总线上有干扰的证据)
"""

import re
import sys
from collections import Counter

# ---- 协议常量 (见 niu_u1_foc_simulator/src/bms_protocol.h 和 NOTES.md §3) ----
ADDRS = {
    (0x10, 0xEF): '仪表',
    (0x20, 0xDF): 'FOC (本机)',
    (0x31, 0xCE): 'BMS',
    (0x40, 0xBF): 'V35 中控',
}

# V35 单次轮询的超时预算 (抓包量出来的: 从发问到收到应答稳定 21ms)
V35_TIMEOUT_MS = 20

BMS_STATUS_LEN = 15
BMS_CELLS_LEN = 40
BMS_MODEL_LEN = 36
BMS_INFO_LEN = 43
BMS_FULL_LEN = 55      # 状态(15) + 单体(40) 拼在一帧里 (2026-11-06 真车日志发现)


def bms_status_of(data):
    """15 字节状态段 → 字典. 布局见 src/bms_protocol.h:
    [0..1] 总电压(大端, 0.1V) [2..4] 未知 [5] 电流(A) [6] 电量% [7..9] 未知 [10..14] 5个温度"""
    if len(data) < BMS_STATUS_LEN:
        return None
    cur = data[5] if data[5] < 128 else data[5] - 256
    return {
        'voltage': (data[0] << 8 | data[1]) / 10.0,
        'current': cur,
        'soc': data[6],
        'temps': list(data[10:15]),
    }


def bms_cells_of(data):
    """40 字节单体段 → 单体电压列表 (mV). 前面是大端 u16, 到第一个 0 为止"""
    out = []
    for i in range(0, len(data) - 1, 2):
        mv = (data[i] << 8) | data[i + 1]
        if mv == 0:
            break
        out.append(mv)
    return out


class Frame(object):
    def __init__(self, direction, t_ms, raw):
        self.dir = direction
        self.t = t_ms
        self.raw = raw

    @property
    def addr_pair(self):
        return (self.raw[1], self.raw[2]) if len(self.raw) >= 3 else (0, 0)

    @property
    def addr_name(self):
        return ADDRS.get(self.addr_pair, '未知(%02X%02X)' % self.addr_pair)

    @property
    def func(self):
        return self.raw[4] if len(self.raw) >= 5 else 0

    @property
    def is_reply(self):
        return bool(self.func & 0x80)

    @property
    def data(self):
        # 数据区每个字节 +0x33 混淆, 这里 -0x33 反混淆
        n = self.raw[5] if len(self.raw) >= 6 else 0
        return [(b - 0x33) & 0xFF for b in self.raw[6:6 + n]]

    def checksum_ok(self):
        if len(self.raw) < 9:
            return False
        s = 0
        for b in self.raw[:-2]:
            s += b
        return (s & 0xFF) == self.raw[-2]

    def length_ok(self):
        if len(self.raw) < 6:
            return False
        return self.raw[5] + 8 == len(self.raw)


def parse_log(path):
    frames = []
    header = []
    app = {}          # App 侧最后收到的遥测 (写在文件头里)
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('#'):
                header.append(line)
                # "# APP 车速 19.6 km/h | 协议车速 41 | 收到于 ..." → App 侧最后收到的值
                if line.startswith('# APP'):
                    m = re.search(r'车速\s*([\d.]+)\s*km/h', line)
                    if m:
                        app['speed'] = float(m.group(1))
                    m = re.search(r'协议车速\s*(\d+)', line)
                    if m:
                        app['protocolSpeed'] = int(m.group(1))
                    m = re.search(r'收到于\s*(.+)$', line)
                    if m:
                        app['at'] = m.group(1).strip()
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            direction = parts[0]
            if direction not in ('RX', 'TX'):
                continue
            try:
                t = int(parts[1])
            except ValueError:
                continue
            try:
                raw = [int(x, 16) for x in parts[2:]]
            except ValueError:
                continue
            frames.append(Frame(direction, t, raw))
    return header, frames, app


def median(values):
    if not values:
        return 0
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) // 2


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    header, frames, app = parse_log(path)

    print('=' * 64)
    for h in header:
        print(h)
    print('=' * 64)

    if not frames:
        print('日志里没有帧 —— 板子上没记到东西.')
        print('可能的原因: 485 没接上 / 车没通电 (V35 没在轮询) / 固件版本太老 (< 1.5.0 没有日志功能)')
        return

    rx = [f for f in frames if f.dir == 'RX']
    tx = [f for f in frames if f.dir == 'TX']
    span = frames[-1].t - frames[0].t

    print()
    print('=== 概览 ===')
    print('时间跨度 : %.2f 秒 (%d ms)' % (span / 1000.0, span))
    print('帧数     : %d  (RX %d / TX %d)' % (len(frames), len(rx), len(tx)))
    if span > 0:
        print('总线速率 : %.1f 帧/秒' % (len(frames) * 1000.0 / span))
    print('起止时刻 : %d ms → %d ms' % (frames[0].t, frames[-1].t))

    # ---- 按设备分组的帧数 ----
    print()
    print('=== 总线上各设备的帧数 ===')
    c = Counter(f.addr_name for f in rx)
    for name, n in c.most_common():
        print('  %-12s %5d 帧' % (name, n))

    # ---- 我们的应答延迟 ----
    # V35 问 FOC (地址对 20 DF, 功能码 0x02 查询, 数据 05 08) → 我们回 0x82.
    # 延迟 = 我们的 TX 时刻 - 那次请求的 RX 时刻.
    print()
    print('=== V35 轮询 → 我们的应答 (错误码 190 的直接原因) ===')
    queries = []
    for f in rx:
        if f.addr_pair == (0x20, 0xDF) and f.func == 0x02:
            queries.append(f)
    # 我们的应答: 地址对 20 DF, 功能码 0x82
    replies = [f for f in tx if f.addr_pair == (0x20, 0xDF) and f.func == 0x82]

    lat = []
    used = set()
    for q in queries:
        best = None
        for i, r in enumerate(replies):
            if i in used:
                continue
            dt = r.t - q.t
            if dt < 0:
                continue
            if best is None or dt < best[0]:
                best = (dt, i)
        if best is not None:
            used.add(best[1])
            lat.append(best[0])

    print('V35 查询 %d 次, 我们应答 %d 次' % (len(queries), len(replies)))
    if lat:
        over = [x for x in lat if x > V35_TIMEOUT_MS]
        print('应答延迟 : 最小 %d ms / 中位 %d ms / 最大 %d ms'
              % (min(lat), median(lat), max(lat)))
        print('超过 %d ms (V35 的超时阈值) 的: %d 次 %s'
              % (V35_TIMEOUT_MS, len(over),
                 '← 就是它让 ECU 报错 190、车速显示 0' if over else '(都赶得上)'))
        if over:
            print('  超时的那些: %s ms' % ', '.join(str(x) for x in sorted(over)[:20]))
    else:
        print('没配对上查询和应答 —— 看下面的原始帧找原因.')

    # ---- 车速: 固件解析出来的 vs App 侧收到的 ----
    print()
    print('=== 车速对账 ===')
    speeds = []      # (t, 协议车速值)
    for f in frames:
        r = f.raw
        if f.dir == 'TX' and r[1] == 0x20 and r[2] == 0xDF and r[4] == 0x82 and r[5] >= 12:
            speeds.append((f.t, (r[6 + 9] - 0x33) & 0xFF))
    if speeds:
        vals = [s for _, s in speeds]
        print('固件解析值: %d 帧, 协议车速 %d ~ %d (%.1f ~ %.1f km/h)'
              % (len(speeds), min(vals), max(vals),
                 min(vals) * 12 / 25, max(vals) * 12 / 25))
        print('  最后一次 (t=%d ms): 协议车速 %d = %.1f km/h'
              % (speeds[-1][0], speeds[-1][1], speeds[-1][1] * 12 / 25))
        if app.get('speed') is not None:
            fw = speeds[-1][1] * 12 / 25
            diff = abs(app['speed'] - fw)
            print('App 侧最后收到: %.1f km/h (协议车速 %s) —— %s'
                  % (app['speed'], app.get('protocolSpeed', '?'),
                     '跟固件一致 ✓' if diff < 1.0 else
                     '**跟固件对不上 → App 侧是陈旧数据, 蓝牙通知链断了**'))
            if diff >= 1.0:
                print('  差 %.1f km/h. 固件这边明明在变, App 这边没更新 → '
                      '不是测速的问题, 是通知没送到' % diff)
        else:
            print('App 侧: 日志头里没有 App 的速度 (没收到过遥测?)')
    else:
        print('日志里没有我们的状态应答帧 (TX), 没法对账')

    # ---- BMS ----
    # 地址对 31 CE 的帧有两种: V35 问 BMS 的请求 (功能码 0x02), 和 BMS 回 V35 的应答 (0x82).
    # 只有后者才说明"BMS 回话了" —— 分开数, 不然会把请求当成应答.
    print()
    print('=== BMS ===')
    bms_all = [f for f in rx if f.addr_pair == (0x31, 0xCE)]
    bms = [f for f in bms_all if f.is_reply]
    bms_queries = [f for f in bms_all if not f.is_reply]
    print('V35 问 BMS %d 次, BMS 回话 %d 帧' % (len(bms_queries), len(bms)))
    if not bms:
        print('  BMS 一帧都没回 —— 电池压根没在总线上应答.')
        print('  查: 电池插好了吗 / V35 在不在轮询它 / 我们的分帧是不是把它吃了')
        print('      (看上面「各设备的帧数」里有没有仪表的帧 —— 有仪表没 BMS 就是电池侧的问题)')

    cs_err = len([f for f in bms if not f.checksum_ok()])
    len_err = len([f for f in bms if not f.length_ok()])
    print('校验和错 %d 帧, 长度不符 %d 帧' % (cs_err, len_err))
    if cs_err:
        print('  校验和错 = 总线上有干扰, 或者两台设备同时说话 (撞车了).')
        print('  撞车的常见原因: 我们应答晚了, 跟 BMS 的应答撞在一起 —— 看上面的延迟.')

    # 55 字节的帧里同时含状态和单体电压 (这台车的 BMS 就是这么回的)
    status = [f for f in bms if f.length_ok() and len(f.data) == BMS_STATUS_LEN]
    cells = [f for f in bms if f.length_ok() and len(f.data) == BMS_CELLS_LEN]
    full = [f for f in bms if f.length_ok() and len(f.data) == BMS_FULL_LEN]
    models = [f for f in bms if f.length_ok() and len(f.data) in (BMS_MODEL_LEN, BMS_INFO_LEN)]
    other = [f for f in bms if f.length_ok() and len(f.data) not in
             (BMS_STATUS_LEN, BMS_CELLS_LEN, BMS_MODEL_LEN, BMS_INFO_LEN, BMS_FULL_LEN)]
    print('应答里: 状态帧 %d, 单体电压帧 %d, 状态+单体(55字节) %d, 型号帧 %d, 数据长度没见过 %d'
          % (len(status), len(cells), len(full), len(models), len(other)))
    if other:
        print('  没见过的数据长度: %s'
              % ', '.join('%d 字节' % len(f.data) for f in other[:10]))
    if full:
        print('  → 55 字节那种是「15 字节状态 + 40 字节单体电压」拼在一起')

    # 状态帧 (15 字节的单独状态, 或 55 字节那种的前 15 字节)
    st_frames = list(status)
    if full:
        st_frames += full
    if st_frames:
        st = bms_status_of(st_frames[-1].data)
        print()
        print('最近一次状态 (t=%d ms):' % st_frames[-1].t)
        print('  总电压 %.1f V   电流 %d A   电量 %d%%'
              % (st['voltage'], st['current'], st['soc']))
        print('  温度 %s' % ' / '.join('%d°' % t for t in st['temps']))
        # 状态帧间隔 —— 正常是 200~350ms 一次
        if len(st_frames) > 2:
            gaps = [st_frames[i].t - st_frames[i - 1].t for i in range(1, len(st_frames))]
            print('  状态帧间隔: 中位 %d ms, 最大 %d ms'
                  % (median(gaps), max(gaps)))

    # 单体电压 (40 字节的单独单体帧, 或 55 字节那种的后 40 字节)
    cell_frames = list(cells)
    if full:
        cell_frames += full
    if cell_frames:
        d = cell_frames[-1].data
        mvs = bms_cells_of(d[BMS_STATUS_LEN:] if len(d) == BMS_FULL_LEN else d)
        if mvs:
            print()
            print('最近一次单体电压 (t=%d ms):' % cell_frames[-1].t)
            print('  %d 串, %.3f ~ %.3f V, 合计 %.2f V'
                  % (len(mvs), min(mvs) / 1000.0, max(mvs) / 1000.0,
                     sum(mvs) / 1000.0))
            print('  ' + ' '.join('%.3f' % (m / 1000.0) for m in mvs))

    if models:
        d = models[-1].data
        off = 7 if len(d) == BMS_INFO_LEN else 0
        s = ''.join(chr(c) for c in d[off:off + 16]
                    if 0x20 <= c <= 0x7E).split('\x00')[0]
        print()
        print('最近一次型号帧: %s' % (s or '(没解出来)'))

    # ---- 别的异常 ----
    print()
    print('=== 其它 ===')
    funcs = Counter(f.func for f in rx)
    print('收到的功能码: %s'
          % ', '.join('0x%02X×%d' % (k, v) for k, v in sorted(funcs.items())))
    odd = [f for f in rx if f.addr_pair not in ADDRS]
    if odd:
        print('地址对不认识的有 %d 帧 (总线上有别的东西在说话?)' % len(odd))

    # 时间戳跳变 —— 板子被复位过? 或日志被冻过?
    jumps = [(frames[i].t - frames[i - 1].t) for i in range(1, len(frames))]
    if jumps:
        big = max(jumps)
        print('相邻两帧最大间隔 %d ms' % big)
        if big > 2000:
            print('  间隔这么大多半是板子复位了 (或者日志传输时冻结了几秒)')

    print()
    print('提示: 完整协议见仓库 niu_u1_foc_simulator/NOTES.md §3, '
          'BMS 帧布局见 §6.6')


if __name__ == '__main__':
    main()
