#!/bin/bash
# тест выбора цвета и запоминания настроек
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgtest
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/c_col.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" >/tmp/c_col.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"

xdotool mousemove $((X + 370)) $((Y + 22)) click 1     # 2-й цвет (зелёный)
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 300)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X + 300 + i * 25)) $((Y + 300 + i * 8)); done
xdotool mouseup 1
sleep 0.3
xdotool mousemove $((X + 460)) $((Y + 22)) click 2     # правый клик — ничего не ломает
sleep 0.2
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1   # готово
sleep 1.2
echo "--- лог:"; cat /tmp/c_col.log
echo "--- конфиг:"; cat "$XDG_CONFIG_HOME/xsnip/config" 2>/dev/null || echo "нет"
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/c_col.png')
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
d = pb.get_pixels()
def px(x,y):
    i = y*stride + x*nc
    return "#%02x%02x%02x" % (d[i], d[i+1], d[i+2])
# ищем зелёные пиксели пера (не плашки): цвет #2f9e44
greens = {}
for y in range(0, h):
    row = d[y*stride:(y+1)*stride]
    for x in range(0, w):
        i = x*nc
        c = (row[i], row[i+1], row[i+2])
        if c[1] > 100 and c[0] < 90 and c[2] < 110:
            greens[c] = greens.get(c, 0) + 1
top = sorted(greens.items(), key=lambda kv: -kv[1])[:3]
print("зелёные оттенки:", [("#%02x%02x%02x" % c, n) for c, n in top])
PY
