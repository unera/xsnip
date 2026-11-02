#!/bin/bash
# тест на большом экране (картинка больше окна): проверка масштабирования координат
set -u
export DISPLAY=:98
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/big.png
pkill -f "Xvfb :98" 2>/dev/null
sleep 0.5
Xvfb :98 -screen 0 3000x2000x24 >/tmp/xvfb98.log 2>&1 &
sleep 1.5
FW=3000 FH=2000 setsid python3 "$HERE/fake_screen.py" >/tmp/fake98.log 2>&1 &
sleep 1.5
xwininfo -root | grep -E "Width|Height"

rm -f "$OUT"
XSNIP_DEBUG=1 setsid python3 "$HERE/../xsnip.py" -o "$OUT" >/tmp/big.log 2>&1 &
echo $! > /tmp/shot98.pid
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно редактора ${WIDTH}x${HEIGHT}"
xdotool mousemove 1000 1000 sleep 0.2 click 1
sleep 0.4
xdotool mousemove $((WIDTH - 45)) 22 click 1
sleep 1.5
cat /tmp/big.log
python3 - <<'EOF'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/big.png')
print("PNG %dx%d" % (pb.get_width(), pb.get_height()))
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
data = pb.get_pixels(); xs=[]; ys=[]
for y in range(h):
    row=data[y*stride:(y+1)*stride]
    for x in range(w):
        i=x*nc
        if row[i]>180 and row[i+1]<110 and row[i+2]<110:
            xs.append(x); ys.append(y)
if xs: print("точка: %.1f,%.1f" % ((min(xs)+max(xs))/2, (min(ys)+max(ys))/2))
else: print("красного нет")
EOF
