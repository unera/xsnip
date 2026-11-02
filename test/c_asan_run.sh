#!/bin/bash
# ASAN-прогон основных сценариев: штрих+файл, текст+буфер, выделение области
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgasan
HERE=$(cd "$(dirname "$0")" && pwd)
A="$HERE/../xsnip_asan"
pkill -x xsnip_asan 2>/dev/null
pkill -f "xclip -selection" 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f /tmp/a1.png /tmp/a1.log /tmp/a2.log /tmp/a3.log

echo "=== 1. штрих + сохранение"
setsid "$A" -o /tmp/a1.png >/tmp/a1.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X+200)) $((Y+300)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X+200+i*30)) $((Y+300+i*12)); done
xdotool mouseup 1
xdotool key 1
sleep 0.2
xdotool mousemove $((X+WIDTH-40)) $((Y+22)) click 1
sleep 1.5
grep -c "ERROR" /tmp/a1.log || true
sed -n '1,6p' /tmp/a1.log

echo "=== 2. текст + буфер (без -o)"
setsid "$A" >/tmp/a2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X+175)) $((Y+22)) click 1
sleep 0.2
xdotool mousemove $((X+300)) $((Y+250)) click 1
sleep 0.5
xdotool type --delay 60 "asan"
xdotool key Return
sleep 0.4
xdotool key ctrl+z
sleep 0.3
xdotool mousemove $((X+WIDTH-40)) $((Y+22)) click 1
sleep 2
sed -n '1,8p' /tmp/a2.log

echo "=== 3. выделение области (-s) + Esc"
setsid "$A" -s -o /tmp/a3.png >/tmp/a3.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip-select$" | head -1)
xdotool mousemove 200 200 mousedown 1
for i in $(seq 0 8); do xdotool mousemove $((200+i*30)) $((200+i*20)); done
xdotool mouseup 1
sleep 1.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool key Escape
sleep 1
sed -n '1,8p' /tmp/a3.log
echo "=== файл a3 (не должен появиться):"; ls -l /tmp/a3.png 2>/dev/null || echo "нет — верно"
