#!/usr/bin/env bash
# 烧录固件到天问 Pro TW002 (ESP32-C3-DevKitC-02)
#
# 板子的 CH340K 没把 GPIO9 (BOOT) 接到 DTR, 软件复位进不了下载模式, 必须手动:
#   1. 拔掉 USB
#   2. 按住 BOOT 不放
#   3. 插回 USB (进下载模式)
#   4. 立刻跑这个脚本
#   5. 看到 "Writing at 0x..." 之后松开 BOOT
#
# 用法: bash tools/flash.sh [串口]   (默认 /dev/ttyUSB0)

set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../niu_u1_foc_simulator" && pwd)"
BUILD="$PROJ/.pio/build/esp32-c3-devkitc-1"

# esptool 优先用 PlatformIO 自带的 (v4.11), 找不到就用系统里的 (v4.7, CLI 一样)
ESPTOOL=""
for cand in "$HOME"/.platformio/packages/tool-esptoolpy/esptool.py \
            "$(command -v esptool.py 2>/dev/null)" \
            "$(command -v esptool 2>/dev/null)"; do
    if [ -n "$cand" ] && [ -f "$cand" ]; then ESPTOOL="$cand"; break; fi
done
[ -n "$ESPTOOL" ] || { echo "找不到 esptool (装一下: pip install esptool)"; exit 1; }
echo "[flash] 用 esptool: $ESPTOOL"
for f in bootloader.bin partitions.bin firmware.bin; do
    [ -f "$BUILD/$f" ] || { echo "缺 $BUILD/$f, 先跑 cd $PROJ && pio run"; exit 1; }
done

echo "[flash] 等串口 $PORT ..."
for _ in $(seq 1 40); do
    [ -e "$PORT" ] && break
    sleep 0.25
done
[ -e "$PORT" ] || { echo "[flash] 串口 $PORT 没出现"; exit 1; }

echo "[flash] 开始烧录 (分区表 + bootloader + app)"
echo "[flash] 连不上就按住 BOOT 重新插一下 USB, 脚本会重试"

# 先擦掉 otadata (0xE000, 0x2000).
# 改了分区表之后必须这么干 —— 旧的 otadata 还记着"从 app1 启动", 而 app1 里
# 现在是新分区表下没刷过的垃圾, bootloader 会照着 otadata 去启动它, 表现为"刷完不启动".
# 擦成全 0xFF 之后 bootloader 找不到有效 otadata, 就退回启动第一个 app 槽 (app0).
for attempt in $(seq 1 30); do
    if python3 "$ESPTOOL" --chip esp32c3 --port "$PORT" --baud 460800 \
        erase_region 0xE000 0x2000; then
        echo "[flash] otadata 已擦除 (启动槽回到 app0)"
        break
    fi
    echo "[flash] 擦 otadata 没连上, 重试"
    sleep 2
done

for attempt in $(seq 1 30); do
    if python3 "$ESPTOOL" \
        --chip esp32c3 --port "$PORT" --baud 460800 \
        --before default_reset --after hard_reset \
        write_flash \
        0x0     "$BUILD/bootloader.bin" \
        0x8000  "$BUILD/partitions.bin" \
        0x10000 "$BUILD/firmware.bin"; then
        break
    fi
    echo "[flash] 第 $attempt 次没连上, 2 秒后重试 (按住 BOOT 再插 USB)"
    sleep 2
    if [ "$attempt" = "30" ]; then
        echo "[flash] 30 次都没连上, 放弃"
        exit 1
    fi
done

echo
echo "[flash] 完成, 板子会自己重启"
echo "[flash] 打开串口 (115200 8N1) 应该看到:"
echo "        === 小牛U1 FOC模拟器 ==="
echo "        [OTA] 热点 NIU-U1-OTA 已启动  IP 192.168.4.1:8080"
echo "[flash] 这次刷的是 A/B 双槽分区表, 刷完 OTA 才能用"
