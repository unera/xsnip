#!/bin/bash
# скорость ресайза: 60 изменений размера подряд не должны занимать секунды
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgresize
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f /tmp/resize.png

setsid "$APP" -o /tmp/resize.png >/tmp/resize.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/resize.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "стартовое окно ${WIDTH}x${HEIGHT}"

T0=$(date +%s%N)
for i in $(seq 1 60); do
  xdotool windowsize "$WID" $((900 + i * 4)) $((500 + i * 3))
done
T1=$(date +%s%N)
sleep 1
echo "60 изменений размера: $(( (T1 - T0) / 1000000 )) мс"

eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "после ресайза окно ${WIDTH}x${HEIGHT}"
# после остановки кэш должен восстановиться: рисуем штрих и сохраняем
xdotool mousemove $((X + 100)) $((Y + 200)) mousedown 1
sleep 0.15
for i in $(seq 0 8); do xdotool mousemove $((X + 100 + i * 20)) $((Y + 200)); sleep 0.02; done
xdotool mouseup 1
sleep 0.4
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('/tmp/resize.png');print('PNG после ресайза:',pb.get_width(),'x',pb.get_height())"
python3 "$HERE/snapshot.py" "$WID" /tmp/resize_win.png 2>/dev/null || true
