#!/bin/bash
# тест CLI-опций: палитра, активный цвет, размеры, тип окна, позиция
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgargs
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/c_args.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" --colors "#0000ff,#00ff00,#ffff00" --color 2 \
    --pen 3 --text 20 --window normal --pos tl >/tmp/c_args.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно: pos=${X},${Y} size=${WIDTH}x${HEIGHT} (ожидание tl = 10,10)"
xdotool mousemove $((X + 300)) $((Y + 300)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X + 300 + i * 20)) $((Y + 300)); done
xdotool mouseup 1
sleep 0.3
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
echo "--- лог:"; cat /tmp/c_args.log
echo "--- конфиг:"; cat "$XDG_CONFIG_HOME/xsnip/config" 2>/dev/null || echo "нет"
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/c_args.png')
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
d = pb.get_pixels()
cols = {}
for y in range(0, h, 2):
    row = d[y*stride:(y+1)*stride]
    for x in range(0, w, 2):
        i = x*nc
        c = (row[i], row[i+1], row[i+2])
        cols[c] = cols.get(c, 0) + 1
top = [("#%02x%02x%02x" % c, n) for c, n in sorted(cols.items(), key=lambda kv: -kv[1])[:5]]
print("PNG %dx%d, частые цвета:" % (w,h), top)
PY
