#!/usr/bin/env bash
# 编译固件 + 把固件打包进小程序
#
# 产物:
#   miniprogram/firmware/firmware.js   给小程序推送用 (模块里带压缩+base64 的固件)
#
# 为什么是 .js 不是 .bin:
#   小程序代码包只收白名单扩展名, .bin / .txt 不在白名单里, 放进去会被悄悄丢掉。
#   所以把固件转成 base64 塞进一个 .js 模块 —— .js 一定进包。
#
# 为什么还要压缩:
#   小程序主包上限 2MB。固件加了 BLE 之后 1.6MB, 直接 base64 是 2.1MB, 超限。
#   先用 tools/lz_pack.py 压到 ~1.05MB, 再 base64 是 1.4MB, 装得下。
#   解压在小程序里做 (miniprogram/utils/lz.js)。
#
# 用法: bash tools/build_ota.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$ROOT/niu_u1_foc_simulator"
BUILD="$PROJ/.pio/build/esp32-c3-devkitc-1"
OUT="$ROOT/miniprogram/firmware"

cd "$PROJ"
pio run

VERSION="$(grep -oP '#define\s+OTA_FW_VERSION\s+"\K[^"]+' src/ota_config.h)"
[ -n "$VERSION" ] || { echo "从 src/ota_config.h 里读不到 OTA_FW_VERSION"; exit 1; }

mkdir -p "$OUT"

SIZE=$(stat -c%s "$BUILD/firmware.bin")
MD5=$(md5sum "$BUILD/firmware.bin" | cut -d' ' -f1)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo
echo "[build] 压缩固件 ..."
python3 "$ROOT/tools/lz_pack.py" "$BUILD/firmware.bin" "$TMP/firmware.lz"
PACKED=$(stat -c%s "$TMP/firmware.lz")

{
  printf '// 自动生成, 别手改 —— tools/build_ota.sh 的产物\n'
  printf '// b64 里是压缩过的固件, 小程序里用 utils/lz.js 解压\n'
  printf 'module.exports = {\n'
  printf "  version: '%s',\n" "$VERSION"
  printf '  size: %s,\n' "$SIZE"
  printf "  md5: '%s',\n" "$MD5"
  printf '  packedSize: %s,\n' "$PACKED"
  printf "  b64: '"
  base64 -w0 "$TMP/firmware.lz"
  printf "',\n}\n"
} > "$OUT/firmware.js"

PKG=$(stat -c%s "$OUT/firmware.js")

echo "[build] 版本    : $VERSION"
echo "[build] 大小    : $SIZE 字节"
echo "[build] md5     : $MD5"
echo "[build] 压缩后  : $PACKED 字节"
echo "[build] 输出    : $OUT/firmware.js ($PKG 字节, 小程序主包上限 2MB)"
