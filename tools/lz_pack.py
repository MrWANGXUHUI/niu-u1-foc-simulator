#!/usr/bin/env python3
"""固件压缩 — 极简 LZ77 变体, 跟 miniprogram/utils/lz.js 的解码器配套.

小程序主包上限 2MB, 固件 base64 后是 2.1MB, 超了. 压缩之后再 base64 就装得下.

格式 (一串 group):
  flag 字节: bit0..bit7, 1 = 后面跟 1 个字面量字节, 0 = 后面跟 3 字节的匹配
  匹配 3 字节: offset (uint16 LE, 往回看多少字节) + len (uint8, 实际长度 = len + 3)

窗口 64KB, 匹配长度 3..258.
"""
import sys

MIN_MATCH = 3
MAX_MATCH = 258
WINDOW = 65535
MAX_CHAIN = 64      # 每个 hash 位置最多往前找多少个候选


def compress(src: bytes) -> bytes:
    n = len(src)
    out = bytearray()
    head = {}                       # 3 字节 -> 最近的位置
    prev = [-1] * (n + 1)           # 同 hash 的上一个位置

    group_flag = 0
    group_items = []                # (是否匹配, a, b): 字面量 (False, 字节, 0), 匹配 (True, offset, 长度)

    def flush_group():
        nonlocal group_flag, group_items
        if not group_items:
            return
        out.append(group_flag)
        for is_match, a, b in group_items:
            if is_match:
                out.append(a & 0xFF)
                out.append((a >> 8) & 0xFF)
                out.append(b - MIN_MATCH)
            else:
                out.append(a)
        group_flag = 0
        group_items = []

    def push_literal(b):
        nonlocal group_flag
        if len(group_items) == 8:
            flush_group()
        group_flag |= 1 << len(group_items)
        group_items.append((False, b, 0))

    def push_match(off, length):
        if len(group_items) == 8:
            flush_group()
        group_items.append((True, off, length))

    def find_match(ip):
        """返回 (长度, 距离), 没有就 (0, 0)"""
        if ip + MIN_MATCH > n:
            return 0, 0
        key = src[ip:ip + MIN_MATCH]
        cand = head.get(key)
        if cand is None:
            return 0, 0
        max_len = min(MAX_MATCH, n - ip)
        best_len = 0
        best_off = 0
        depth = 0
        while cand >= 0 and depth < MAX_CHAIN:
            off = ip - cand
            if off > WINDOW:
                break
            # 先看一眼 best_len 那一字节, 不一样就不用比了
            if best_len >= max_len or src[cand + best_len] != src[ip + best_len]:
                cand = prev[cand]
                depth += 1
                continue
            ln = 0
            while ln < max_len and src[cand + ln] == src[ip + ln]:
                ln += 1
            if ln > best_len:
                best_len = ln
                best_off = off
                if best_len >= max_len:
                    break
            cand = prev[cand]
            depth += 1
        if best_len < MIN_MATCH:
            return 0, 0
        return best_len, best_off

    def insert(p):
        if p + MIN_MATCH <= n:
            key = src[p:p + MIN_MATCH]
            prev[p] = head.get(key, -1)
            head[key] = p

    ip = 0
    while ip < n:
        length, off = find_match(ip)
        # 懒惰匹配: 下一个位置的匹配更长的话, 这里就吐一个字面量
        if MIN_MATCH <= length < MAX_MATCH and ip + 1 < n:
            nxt_len, _ = find_match(ip + 1)
            if nxt_len > length:
                push_literal(src[ip])
                insert(ip)
                ip += 1
                continue
        if length >= MIN_MATCH:
            push_match(off, length)
            for k in range(length):
                insert(ip + k)
            ip += length
        else:
            push_literal(src[ip])
            insert(ip)
            ip += 1

    flush_group()
    return bytes(out)


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    packed = compress(data)
    open(sys.argv[2], "wb").write(packed)
    print(f"{len(data)} -> {len(packed)} 字节 ({len(packed) * 100 // len(data)}%)")
