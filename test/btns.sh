#!/bin/bash
# отладка: список кнопок панели и их координаты
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgtest
HERE=$(cd "$(dirname "$0")" && pwd)
pkill -x xsnip 2>/dev/null
sleep 0.5
XSNIP_DEBUG=1 setsid "$HERE/../xsnip" -o /tmp/dbg.png >/tmp/dbg.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
xdotool getwindowgeometry "$WID"
head -14 /tmp/dbg.log
