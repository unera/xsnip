#!/bin/bash
# корректность сдвига: штрих, обрезка, штрих в новых координатах + undo кнопкой
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgcrop3
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/crop3.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" >/tmp/crop4.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно ${WIDTH}x${HEIGHT}"

# горизонтальный штрих на известной высоте
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 10); do xdotool mousemove $((X + 200 + i * 20)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.4

# обрезаем область (300,250)-(600,450)
xdotool key 3
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 250)) mousedown 1
sleep 0.15
for i in $(seq 0 10); do xdotool mousemove $((X + 300 + i * 30)) $((Y + 250 + i * 20)); sleep 0.03; done
python3 "$HERE/snapshot.py" "$WID" /tmp/crop_xor.png
xdotool mouseup 1
sleep 0.8
xdotool key 1
sleep 0.2
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2

python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/crop3.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels()
xs=[];ys=[]
for y in range(h):
    row=d[y*s:(y+1)*s]
    for x in range(w):
        i=x*nc
        if row[i]>150 and row[i+1]<120 and row[i+2]<120:
            xs.append(x);ys.append(y)
print("PNG %dx%d" % (w,h))
if xs:
    print("штрих: x %d..%d, y %d..%d (ожидание y ~63, x 0..127)" % (min(xs),max(xs),min(ys),max(ys)))
else:
    print("штриха нет — плохо")
PY

# теперь undo кнопкой: штрих должен вернуться на прежнее место
rm -f "$OUT"
pkill -x xsnip 2>/dev/null; sleep 0.5
setsid "$APP" -o "$OUT" >/tmp/crop5.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 10); do xdotool mousemove $((X + 200 + i * 20)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.4
xdotool key 3
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 250)) mousedown 1
sleep 0.15
for i in $(seq 0 10); do xdotool mousemove $((X + 300 + i * 30)) $((Y + 250 + i * 20)); sleep 0.03; done
xdotool mouseup 1
sleep 0.8
xdotool mousemove $((X + 25)) $((Y + 22)) click 1   # undo-кнопка
sleep 0.8
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/crop3.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels()
xs=[];ys=[]
for y in range(h):
    row=d[y*s:(y+1)*s]
    for x in range(w):
        i=x*nc
        if row[i]>150 and row[i+1]<120 and row[i+2]<120:
            xs.append(x);ys.append(y)
print("после undo кнопкой: PNG %dx%d, штрих y %s..%s (ожидание ~320)" % (
    w,h, min(ys) if ys else "-", max(ys) if ys else "-"))
PY
