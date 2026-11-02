#!/bin/bash
# проверки по ревью: инъекция, портретная панель, backspace по UTF-8, локаль
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-/usr/bin/xsnip}

echo "=== 1. инъекция через XDG_CONFIG_HOME"
rm -f /tmp/PWNED_R
XDG_CONFIG_HOME="/tmp/y'; touch /tmp/PWNED_R; echo '" "$APP" --version >/dev/null 2>&1
if [ -f /tmp/PWNED_R ]; then echo "  ПЛОХО: команда выполнилась"; else echo "  ок: файла нет"; fi

echo "=== 2. портретный монитор 1280x2160: все кнопки внутри окна"
pkill -x xsnip 2>/dev/null || true
sleep 0.5
pkill -f "Xvfb :93" 2>/dev/null || true
sleep 0.5
Xvfb :93 -screen 0 1280x2160x24 >/tmp/x93.log 2>&1 &
sleep 2
export DISPLAY=:93
export LD_PRELOAD="$HERE/fake_xinerama.so"
export XSNIP_FAKE_HEADS="1280x2160+0+0"
export XDG_CONFIG_HOME=/tmp/cfgport
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
XSNIP_DEBUG=1 setsid "$APP" -o /tmp/port.png >/tmp/port.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "  нет окна"; cat /tmp/port.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "  окно ${WIDTH}x${HEIGHT}"
python3 - <<'PY'
import re
lines = [l for l in open('/tmp/port.log') if l.startswith('btn ')]
mx = 0
for l in lines[-14:]:
    m = re.match(r'btn id=(\d+) x=(\d+) w=(\d+)', l)
    if m:
        mx = max(mx, int(m.group(2)) + int(m.group(3)))
print("  правая граница кнопок: %d" % mx)
PY
echo "  готово нажимается?"
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 30)) $((Y + 20)) click 1
sleep 1.2
[ -f /tmp/port.png ] && echo "  ок: файл сохранён" || echo "  ПЛОХО: клик по «готово» не сработал"

echo "=== 3. Backspace удаляет символ, а не байт (кириллица)"
pkill -x xsnip 2>/dev/null || true
sleep 0.5
unset LD_PRELOAD
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgbksp
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f /tmp/bksp.png
setsid "$APP" -o /tmp/bksp.png >/tmp/bksp.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1); eval "$(xdotool getwindowgeometry --shell $WID)"
xdotool key t; sleep 0.3
xdotool mousemove $((X + 150)) $((Y + 300)) click 1; sleep 0.4
xdotool type --delay 50 "привет"; sleep 0.3
xdotool key BackSpace BackSpace BackSpace BackSpace; sleep 0.4
xdotool key Return; sleep 0.5
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1; sleep 1.2
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/bksp.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels()
xs=[];ys=[]
for y in range(h):
    row=d[y*s:(y+1)*s]
    for x in range(w):
        i=x*nc
        if row[i]>150 and row[i+1]<120 and row[i+2]<120:
            xs.append(x);ys.append(y)
print("  PNG %dx%d, текст: x %d..%d (ожидание узкой надписи «пр»)" % (w,h,min(xs),max(xs)))
PY
