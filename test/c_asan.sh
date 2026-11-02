#!/bin/bash
# прогон ASAN-версии по сценарию «текст» и разбор падения
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgasan
HERE=$(cd "$(dirname "$0")" && pwd)
pkill -x xsnip 2>/dev/null
pkill -x xsnip_asan 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f /tmp/asan.png /tmp/asan.log

setsid "$HERE/../xsnip_asan" -o /tmp/asan.png >/tmp/asan.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 175)) $((Y + 22)) click 1
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 200)) click 1
sleep 0.8
xdotool type --delay 70 "Hello"
sleep 0.3
xdotool key Return
sleep 1.0
echo "--- asan log:"; head -40 /tmp/asan.log
