#!/usr/bin/env bash
# 项目自测 —— 不用板子, 不用车, 几秒钟跑完.
#
#   bash tools/run_tests.sh
#
# 测什么:
#   1) 设备侧: 485 日志的环形缓冲 / 文本格式 / BLE 包格式 / BMS 报文解析
#   2) 小程序侧: BLE 消息解析 (日志包 / 电池状态 / 电池诊断) + 跨语言还原
#   3) 车辆页: 485 帧分块拼装 / 日志重组与保存 / 趋势图的数据点
#   4) 趋势图的坐标换算
#   5) 离线分析脚本能吃自己生成的日志

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/niu_u1_foc_simulator/src"
TMP="${TMPDIR:-/tmp}/niu485_test"
mkdir -p "$TMP"

echo "=== 1/5 设备侧 (C++) ==="
g++ -Wall -I"$SRC" -o "$TMP/test_bus_log" "$ROOT/tools/test_bus_log.cpp"
"$TMP/test_bus_log" "$TMP/packets.bin" "$TMP/expect.txt"

echo
echo "=== 2/5 小程序侧 (node) ==="
node "$ROOT/tools/test_miniprogram_ble.js" "$TMP/packets.bin" "$TMP/expect.txt"

echo
echo "=== 3/5 车辆页 (node) ==="
node "$ROOT/tools/test_vehicle_page.js"

echo
echo "=== 4/5 趋势图 (node) ==="
node "$ROOT/tools/test_chart.js"

echo
echo "=== 5/5 离线分析脚本 ==="
python3 "$ROOT/tools/analyze_485_log.py" "$TMP/expect.txt" > "$TMP/report.txt"
grep -q "=== 概览 ===" "$TMP/report.txt"
grep -q "BMS 回话" "$TMP/report.txt"
echo "✓ 分析脚本正常 (报告在 $TMP/report.txt)"

echo
echo "全部通过"
