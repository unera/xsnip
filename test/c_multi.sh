#!/bin/bash
# два «монитора» через LD_PRELOAD: снимок активного экрана, размер и место окна
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
export DISPLAY=:94
export XDG_CONFIG_HOME=/tmp/cfgmulti
export LD_PRELOAD="$HERE/fake_xinerama.so"
export XSNIP_FAKE_HEADS="1920x1080+0+0,1920x1200+0+1080"

pkill -x xsnip 2>/dev/null || true
pkill -x Xvfb 2>/dev/null || true
sleep 0.6
Xvfb :94 -screen 0 1920x2280x24 >/tmp/xvfb94.log 2>&1 &
sleep 2
setsid python3 "$HERE/fake_screen.py" >/tmp/fake94.log 2>&1 &
sleep 2
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

echo "=== 1. без -o и без -s: окно на мониторе под курсором (нижний, y=1080..2280)"
rm -f /tmp/m1.png /tmp/m2.png
setsid "$APP" -o /tmp/m1.png --pos tl >/tmp/m1.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/m1.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно ${WIDTH}x${HEIGHT} at ${X},${Y} (нижний монитор начинается с y=1080)"
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.5
python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('/tmp/m1.png');print('снимок:',pb.get_width(),'x',pb.get_height(),'(ожидание 1920x1200 — активный монитор)')"

echo "=== 2. -a: весь виртуальный экран"
pkill -x xsnip 2>/dev/null || true
sleep 0.6
setsid "$APP" -a -o /tmp/m2.png >/tmp/m2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно ${WIDTH}x${HEIGHT} at ${X},${Y} (не больше монитора 1920x1200)"
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 2
python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('/tmp/m2.png');print('снимок:',pb.get_width(),'x',pb.get_height(),'(ожидание 1920x2280 — все)')"
