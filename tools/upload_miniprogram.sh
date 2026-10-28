#!/usr/bin/env bash
# 上传 / 预览微信小程序 (miniprogram-ci 命令行, 不需要开发者工具)
#
# 用法:
#   bash tools/upload_miniprogram.sh preview            # 生成预览二维码 (手机上扫码就能用)
#   bash tools/upload_miniprogram.sh upload 1.0.1 "修 bug"
#
# 前置条件 (缺一不可):
#   1. 公众平台 -> 开发管理 -> 开发设置 -> 小程序代码上传密钥, 下载 private.<appid>.key
#      放到仓库外, 路径通过环境变量 NIU_MINI_KEY 传进来 (别提交进 git!)
#   2. 同一页面的「IP 白名单」要加上这台机器的公网 IP (密钥调用按 IP 鉴权)
#   3. NIU_MINI_APPID 环境变量 = 小程序 AppID
#   4. miniprogram/firmware/firmware.js 存在 (先跑 tools/build_ota.sh),
#      否则小程序里没有可推送的固件
#
# 环境变量:
#   NIU_MINI_APPID   小程序 AppID
#   NIU_MINI_KEY     上传私钥文件路径
#   NIU_MINI_ROBOT   上传机器人编号 1..30, 默认 1

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MP="$ROOT/miniprogram"
ACTION="${1:-upload}"
VERSION="${2:-}"
DESC="${3:-}"

APPID="${NIU_MINI_APPID:-}"
KEY="${NIU_MINI_KEY:-}"
ROBOT="${NIU_MINI_ROBOT:-1}"

if [ -z "$APPID" ] || [ -z "$KEY" ]; then
  echo "缺少前置条件:"
  [ -z "$APPID" ] && echo "  - NIU_MINI_APPID 没设 (小程序 AppID)"
  [ -z "$KEY" ] && echo "  - NIU_MINI_KEY 没设 (代码上传私钥 private.<appid>.key 的路径)"
  echo
  echo "例: NIU_MINI_APPID=wx1234567890abcdef NIU_MINI_KEY=~/keys/private.wx1234567890abcdef.key \\"
  echo "    bash tools/upload_miniprogram.sh upload 1.0.1 '修 bug'"
  exit 1
fi

if [ ! -f "$KEY" ]; then
  echo "找不到私钥文件: $KEY"
  exit 1
fi

if [ ! -f "$MP/firmware/firmware.js" ]; then
  echo "miniprogram/firmware/firmware.js 不存在 —— 小程序里还没有能推送的固件"
  echo "先跑: bash tools/build_ota.sh"
  exit 1
fi

if [ -z "$VERSION" ]; then
  VERSION="$(grep -oP "version: '\K[^']+" "$MP/firmware/firmware.js" 2>/dev/null || echo '')"
  VERSION="${VERSION:-1.0.0}"
fi
[ -n "$DESC" ] || DESC="固件 $VERSION"

if [ ! -d "$ROOT/node_modules/miniprogram-ci" ]; then
  echo "[ci] 安装 miniprogram-ci ..."
  (cd "$ROOT" && npm install --no-audit --no-fund)
fi

# 这台机器是 IPv6 出口, 而公众平台的 IP 白名单里加的是 IPv4 —— 强制走 IPv4
export NODE_OPTIONS="${NODE_OPTIONS:+$NODE_OPTIONS }--dns-result-order=ipv4first"

COMMON=(--appid "$APPID" --project-path "$MP" --private-key-path "$KEY" --robot "$ROBOT")

case "$ACTION" in
  upload)
    echo "[ci] 上传 $VERSION  ($DESC)"
    npx --no-install miniprogram-ci upload "${COMMON[@]}" \
      --upload-version "$VERSION" --upload-description "$DESC"
    echo "[ci] 上传完成 —— 去公众平台「版本管理」把它「设为体验版」再扫码"
    ;;
  preview)
    OUT="$ROOT/miniprogram-preview-qrcode.png"
    echo "[ci] 生成预览二维码 ($VERSION)"
    npx --no-install miniprogram-ci preview "${COMMON[@]}" \
      --upload-version "$VERSION" --upload-description "$DESC" \
      --qrcode-format image --qrcode-output-dest "$OUT"
    echo "[ci] 二维码: $OUT"
    ;;
  *)
    echo "未知动作: $ACTION (只支持 upload / preview)"
    exit 1
    ;;
esac
