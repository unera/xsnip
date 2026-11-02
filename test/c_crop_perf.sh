#!/bin/bash
# 1) скорость вырезания: 200 движений мыши не должны растягиваться в минуты
# 2) отмена обрезки: undo возвращает исходный размер картинки
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgcrop2
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/crop2.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

XSNIP_DEBUG=1 setsid "$APP" -o "$OUT" >/tmp/crop2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/crop2.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
CROP_X=$(grep -o "btn id=5 x=[0-9]*" /tmp/crop2.log | tail -1 | cut -d= -f3)
CROP_W=$(grep -o "btn id=5 x=[0-9]* w=[0-9]*" /tmp/crop2.log | tail -1 | rev | cut -d= -f1 | rev)
xdotool mousemove $((X + CROP_X + CROP_W / 2)) $((Y + 22)) click 1
sleep 0.4

xdotool mousemove $((X + 150)) $((Y + 150)) mousedown 1
sleep 0.2
T0=$(date +%s%N)
for i in $(seq 1 200); do
  xdotool mousemove $((X + 150 + i * 3)) $((Y + 150 + i * 2))
done
T1=$(date +%s%N)
xdotool mouseup 1
sleep 0.5
echo "200 движений мышью в режиме вырезания: $(( (T1 - T0) / 1000000 )) мс"

eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1   # сохранить (обрезано)
sleep 1.2
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/crop2.png')
print("после обрезки: %dx%d" % (pb.get_width(), pb.get_height()))
PY

# теперь то же, но с отменой обрезки
rm -f "$OUT"
pkill -x xsnip 2>/dev/null; sleep 0.5
setsid "$APP" -o "$OUT" >/tmp/crop3.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool key 3           # инструмент «вырезать»
sleep 0.3
xdotool mousemove $((X + 200)) $((Y + 200)) mousedown 1
sleep 0.15
for i in $(seq 0 8); do xdotool mousemove $((X + 200 + i * 30)) $((Y + 200 + i * 20)); sleep 0.02; done
xdotool mouseup 1
sleep 0.6
xdotool key ctrl+z      # отменить обрезку
sleep 0.6
xdotool key 1           # назад к перу
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/crop2.png')
print("после обрезки и Ctrl+Z: %dx%d (ожидание 1280x800)" % (pb.get_width(), pb.get_height()))
PY
