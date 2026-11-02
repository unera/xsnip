#!/bin/bash
# тест текста и запоминания настроек между запусками
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgtext
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../shot"
OUT=/tmp/c_text.png

pkill -x shot 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" --window dialog --pos center >/tmp/c_text.log 2>&1 &
sleep 2
WID=$(xdotool search --name "^shot$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"

xdotool mousemove $((X + 175)) $((Y + 22)) click 1     # кнопка «текст»
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 200)) click 1
sleep 0.8
xdotool type --delay 70 "Shot C version"
sleep 0.3
xdotool key Return
sleep 0.5
echo "--- окно после ввода:"
python3 "$HERE/snapshot.py" "$WID" /tmp/c_text_win.png
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.2
echo "--- конфиг:"; cat "$XDG_CONFIG_HOME/shot/config" 2>/dev/null || echo "нет"
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/c_text.png')
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
d = pb.get_pixels()
# ищем красные пиксели (текст красный по умолчанию) в левой верхней четверти
xs=[]; ys=[]
for y in range(0, h):
    row = d[y*stride:(y+1)*stride]
    for x in range(0, w):
        i = x*nc
        if row[i] > 170 and row[i+1] < 110 and row[i+2] < 110:
            xs.append(x); ys.append(y)
print("PNG %dx%d, красных пикселей: %d" % (w,h,len(xs)))
if xs: print("bbox x %d..%d y %d..%d" % (min(xs),max(xs),min(ys),max(ys)))
PY
