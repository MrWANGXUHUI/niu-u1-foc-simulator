#!/usr/bin/env bash
# 用 curl 模拟微信小程序, 把固件推给 ESP32 —— 固件侧 OTA 的端到端自测
#
# 前置: 电脑连上 ESP32 的热点 NIU-U1-OTA (密码 niu12345)
#       并且板子已经全量刷过 A/B 分区表的固件 (tools/flash.sh)
# 用法: bash tools/ota_push_test.sh [IP] [固件路径]
#   IP 默认 192.168.4.1
#   固件默认 niu_u1_foc_simulator/.pio/build/esp32-c3-devkitc-1/firmware.bin

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IP="${1:-192.168.4.1}"
PORT="${OTA_PORT:-8080}"
FIRMWARE="${2:-$ROOT/niu_u1_foc_simulator/.pio/build/esp32-c3-devkitc-1/firmware.bin}"
CHUNK=4096
BASE="http://$IP:$PORT"

[ -f "$FIRMWARE" ] || { echo "找不到固件 $FIRMWARE (先在 niu_u1_foc_simulator 跑 pio run)"; exit 1; }

TOTAL=$(stat -c%s "$FIRMWARE")
MD5=$(md5sum "$FIRMWARE" | cut -d' ' -f1)

echo "==> 目标 $BASE   固件 $FIRMWARE ($TOTAL 字节)"

echo "==> GET /ota/info"
INFO=$(curl -s --max-time 10 "$BASE/ota/info")
echo "    $INFO"
echo "$INFO" | grep -q '"ok":true' || { echo "设备没响应, 检查是不是连上热点了"; exit 1; }
OLD_VERSION=$(echo "$INFO" | grep -oP '"version":"\K[^"]+')
OLD_SLOT=$(echo "$INFO" | grep -oP '"slot":"\K[^"]+')
echo "    设备当前版本: $OLD_VERSION   运行分区: $OLD_SLOT"

echo "==> POST /ota/begin"
curl -s --max-time 20 -X POST -H "Content-Type: application/json" \
     -d "{\"size\":$TOTAL,\"md5\":\"$MD5\"}" "$BASE/ota/begin"
echo

echo "==> POST /ota/chunk ($CHUNK 字节/块)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
split -b $CHUNK -d -a 4 "$FIRMWARE" "$TMP/chunk_"
OFFSET=0
COUNT=0
for f in "$TMP"/chunk_*; do
    N=$(stat -c%s "$f")
    RESP=$(base64 -w0 "$f" | curl -s --max-time 20 -X POST \
            -H "Content-Type: text/plain" -H "X-Offset: $OFFSET" \
            --data-binary @- "$BASE/ota/chunk")
    case "$RESP" in
        *'"ok":true'*) ;;
        *) echo "    块失败: $RESP"; exit 1 ;;
    esac
    OFFSET=$((OFFSET + N))
    COUNT=$((COUNT + 1))
    if [ $((COUNT % 20)) -eq 0 ] || [ "$OFFSET" -eq "$TOTAL" ]; then
        printf "\r    %d 字节 / %d (%.0f%%)" "$OFFSET" "$TOTAL" "$(echo "$OFFSET $TOTAL" | awk '{print $1*100/$2}')"
    fi
done
echo
echo "==> 传输完成, POST /ota/end"
curl -s --max-time 20 -X POST "$BASE/ota/end"
echo

echo "==> 等设备重启 (5 秒)..."
sleep 5
echo "==> 再次 GET /ota/info"
NEW=$(curl -s --max-time 10 "$BASE/ota/info")
echo "    $NEW"
NEW_VERSION=$(echo "$NEW" | grep -oP '"version":"\K[^"]+')
NEW_SLOT=$(echo "$NEW" | grep -oP '"slot":"\K[^"]+')

# 传的是和当前一样的固件时版本号不会变, 真正的证据是运行分区换了
if [ "$NEW_SLOT" = "$OLD_SLOT" ]; then
    echo "!! 运行分区没变 (还是 $NEW_SLOT), OTA 大概率没生效 —— 查串口日志"
    exit 1
fi
echo "==> OK: 分区 $OLD_SLOT → $NEW_SLOT, 版本 $OLD_VERSION → $NEW_VERSION"
