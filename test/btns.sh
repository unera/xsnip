#!/bin/bash
# отладка: список кнопок панели и их координаты
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgtest
HERE=$(cd "$(dirname "$0")" && pwd)
pkill -x shot 2>/dev/null
sleep 0.5
SHOT_DEBUG=1 setsid "$HERE/../shot" -o /tmp/dbg.png >/tmp/dbg.log 2>&1 &
echo $! > /tmp/shot.pid
sleep 2
WID=$(xdotool search --name "^shot$" | head -1)
xdotool getwindowgeometry "$WID"
head -14 /tmp/dbg.log
