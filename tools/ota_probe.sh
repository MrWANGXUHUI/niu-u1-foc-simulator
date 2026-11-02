#!/usr/bin/env bash
# 只发一个 HTTP 请求, 看板子会不会被这一个请求搞挂.
#
# 排查"发了某个请求板子就重启"这类问题时用: 一次只打一个接口,
# 崩了就是它干的, 没崩就换下一个.
#
# 用法: bash tools/ota_probe.sh /ota/info
# 环境变量: NIU_BLE_MAC 板子的蓝牙 MAC
set -uo pipefail
PATH_="${1:-/ota/info}"
MAC="${NIU_BLE_MAC:-E8:3D:C1:8D:D4:06}"
LOG=/tmp/ota_probe_serial.log

timeout 60 python3 -u -c "
import serial, time
s=serial.Serial('/dev/ttyUSB0',115200,timeout=0.3)
t0=time.time()
while time.time()-t0 < 55:
    try:
        c=s.read(400)
        if c: print(repr(c.decode('utf-8','replace')), flush=True)
    except Exception as e:
        print('err', e, flush=True)
" > "$LOG" 2>&1 &
SPID=$!
sleep 2

(printf 'connect\n'; sleep 4; printf 'char-write-req 0x0033 0x57\n'; sleep 4; printf 'quit\n') \
  | timeout 30 gatttool -b "$MAC" -t 0x2F -I 2>&1 | grep -ai "written" | head -1
sleep 4
nmcli dev disconnect wlp7s0 >/dev/null 2>&1
sleep 2
nmcli dev wifi rescan >/dev/null 2>&1
sleep 4
nmcli dev wifi connect "NIU-U1-OTA" password "niu12345" ifname wlp7s0 >/dev/null 2>&1
sleep 6
echo "[probe] GET $PATH_"
curl -s --max-time 12 "http://192.168.4.1:8080$PATH_"
echo
sleep 3
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null

if grep -aq "panic'ed" "$LOG"; then
  echo "[probe] *** 崩了 ***"
else
  echo "[probe] 没崩"
fi
grep -a "panic'ed" "$LOG" | head -1 | cut -c1-200
