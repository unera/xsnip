#!/bin/bash
# 1) штрих не смещается после отпускания мыши; 2) кнопка «отменить»; 3) маркер вставки текста
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgfix
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/fix.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" >/tmp/fix.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/fix.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно ${WIDTH}x${HEIGHT} at ${X},${Y}"

# горизонтальный штрих слева-направо на фиксированной высоте
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X + 200 + i * 40)) $((Y + 300)); done
xdotool mouseup 1
sleep 0.5

# включаем текст и кликаем, чтобы увидеть маркер вставки
xdotool mousemove $((X + 175)) $((Y + 22)) click 1
sleep 0.3
xdotool mousemove $((X + 120)) $((Y + 450)) click 1
sleep 0.5
python3 "$HERE/snapshot.py" "$WID" /tmp/fix_marker.png
xdotool key Escape    # отменить ввод текста
sleep 0.3

# «отменить» — файла быть не должно
eval "$(xdotool getwindowgeometry --shell "$WID")"
grep "^btn id=7" /tmp/fix.log | tail -1
xdotool mousemove $((X + WIDTH - 160)) $((Y + 22)) click 1
sleep 1
if [ -f "$OUT" ]; then echo "странно: файл появился"; else echo "отмена: файла нет — верно"; fi

# теперь то же, но сохраняем: смотрим, где штрих
rm -f "$OUT"
pkill -x xsnip 2>/dev/null; sleep 0.5
setsid "$APP" -o "$OUT" >/tmp/fix2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool key 1          # перо: первый прогон оставил в конфиге режим текста
sleep 0.3
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.2
for i in $(seq 0 10); do xdotool mousemove $((X + 200 + i * 40)) $((Y + 300)); sleep 0.05; done
xdotool mouseup 1
sleep 0.6
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.5
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/fix.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels()
xs=[];ys=[]
for y in range(h):
    row=d[y*s:(y+1)*s]
    for x in range(w):
        i=x*nc
        if row[i]>170 and row[i+1]<110 and row[i+2]<110:
            xs.append(x);ys.append(y)
print("PNG %dx%d, штрих: x %d..%d, y %d..%d" % (w,h,min(xs),max(xs),min(ys),max(ys)))
print("ожидание: y около 302 (без поправки на панель было бы 344), x 160..630")
PY
