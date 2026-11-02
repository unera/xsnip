#!/bin/bash
# высота поля ввода при двух мониторах: max(высота)/25
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export DISPLAY=:94
export XDG_CONFIG_HOME=/tmp/cfgmulti4
export LD_PRELOAD="$HERE/fake_xinerama.so"
export XSNIP_FAKE_HEADS="1920x1080+0+0,1920x1200+0+1080"

pkill -x xsnip 2>/dev/null || true
pkill -f "Xvfb :9[0-9]" 2>/dev/null || true
sleep 0.6
Xvfb :94 -screen 0 1920x2280x24 >/tmp/x94.log 2>&1 &
sleep 2
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

XSNIP_DEBUG=1 setsid "$HERE/../xsnip" -o /tmp/multi_inp.png >/tmp/multi_inp.log 2>&1 &
sleep 2.5
grep -E "monitor |поле ввода" /tmp/multi_inp.log | head -4
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/multi_inp.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 170)) $((Y + 22)) click 1
sleep 0.3
xdotool mousemove $((X + 200)) $((Y + 400)) click 1
sleep 0.4
xdotool type --delay 50 "два монитора"
sleep 0.3
python3 "$HERE/snapshot.py" "$WID" /tmp/multi_inp_win.png
