#!/usr/bin/env bash
# 开热点 + 电脑连上去 —— OTA 测试前置
#
# 用法: bash tools/ota_open_ap.sh
# 环境变量: NIU_BLE_MAC 板子的蓝牙 MAC (默认是台架上这块)
set -uo pipefail
MAC="${NIU_BLE_MAC:-E8:3D:C1:8D:D4:06}"

echo "[ap] 蓝牙发 'W' 开热点 ($MAC)"
(printf 'connect\n'; sleep 4; printf 'char-write-req 0x0033 0x57\n'; sleep 4; printf 'quit\n') \
  | timeout 30 gatttool -b "$MAC" -t 0x2F -I 2>&1 | grep -ai "written" | head -1
sleep 4

nmcli dev disconnect wlp7s0 >/dev/null 2>&1
sleep 2
nmcli dev wifi rescan >/dev/null 2>&1
sleep 4
nmcli dev wifi connect "NIU-U1-OTA" password "niu12345" ifname wlp7s0 >/dev/null 2>&1
sleep 6
IP=$(ip -4 addr show wlp7s0 | grep -oP '192\.168\.4\.\d+' | head -1)
echo "[ap] 电脑 IP: ${IP:-没拿到}"
[ -n "$IP" ] || exit 1
