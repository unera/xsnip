#!/bin/bash
# скорость выделения области (-s): 200 движений мышью
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgselperf
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}
OUT=/tmp/selperf.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -s -o "$OUT" >/tmp/selperf.log 2>&1 &
sleep 2.5
SEL=$(xdotool search --name "^xsnip-select$" | head -1)
[ -z "$SEL" ] && { echo "FAIL: окна выделения нет"; cat /tmp/selperf.log; exit 1; }

xdotool mousemove 100 100 mousedown 1
sleep 0.2
T0=$(date +%s%N)
for i in $(seq 1 200); do
  xdotool mousemove $((100 + i * 4)) $((100 + i * 3))
done
T1=$(date +%s%N)
sleep 0.4                      # дать дорисовать полный кадр
python3 "$HERE/snapshot.py" "$SEL" /tmp/selperf_frame.png
xdotool mouseup 1
sleep 1.5
echo "200 движений в селекторе: $(( (T1 - T0) / 1000000 )) мс"

WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -z "$WID" ]; then echo "FAIL: редактор не открылся"; cat /tmp/selperf.log; exit 1; fi
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG %dx%d' % (pb.get_width(), pb.get_height()))"
