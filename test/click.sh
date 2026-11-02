#!/bin/bash
# один клик в известную точку — проверить попадание координат
set -u
export DISPLAY=:99 SHOT_DEBUG=1
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/click.png
[ -f /tmp/shot.pid ] && kill "$(cat /tmp/shot.pid)" 2>/dev/null
sleep 0.3
rm -f "$OUT"
setsid python3 "$HERE/../shot.py" -o "$OUT" >/tmp/shot2.log 2>&1 &
echo $! > /tmp/shot.pid
sleep 2
WID=$(xdotool search --name "^shot$" | head -1)
xdotool mousemove 600 400 sleep 0.2 click 1
sleep 0.4
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "window ${WIDTH}x${HEIGHT}"
xdotool mousemove $((WIDTH - 45)) 22 click 1
sleep 1
cat /tmp/shot2.log
python3 - <<'EOF'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/click.png')
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
data = pb.get_pixels()
xs=[];ys=[]
for y in range(h):
    row=data[y*stride:(y+1)*stride]
    for x in range(w):
        i=x*nc
        if row[i]>180 and row[i+1]<110 and row[i+2]<110:
            xs.append(x);ys.append(y)
if xs:
    print("точка: центр %.1f,%.1f  bbox x %d..%d y %d..%d" % ((min(xs)+max(xs))/2,(min(ys)+max(ys))/2,min(xs),max(xs),min(ys),max(ys)))
else:
    print("красного нет вообще")
EOF
