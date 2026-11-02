#!/bin/bash
# после ресайза картинка должна вернуться (не остаться серой заливкой)
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgresize2
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
setsid "$APP" >/tmp/resize2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"

xdotool windowsize "$WID" 700 500
sleep 0.4   # больше 120 мс: кэш должен пересобраться
python3 "$HERE/snapshot.py" "$WID" /tmp/resize2_win.png >/dev/null
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/resize2_win.png')
w,h,s,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
d = pb.get_pixels()
# считаем, сколько пикселей отличаются от серой заливки 0x9a9a96
grey = (0x9a, 0x9a, 0x96)
diff = 0; total = 0
for y in range(50, h, 3):
    row = d[y*s:(y+1)*s]
    for x in range(0, w, 3):
        i = x*nc
        total += 1
        if (row[i], row[i+1], row[i+2]) != grey:
            diff += 1
print("окно %dx%d, пикселей не-серых: %.0f%%" % (w, h, 100.0*diff/total))
PY
pkill -x xsnip 2>/dev/null
