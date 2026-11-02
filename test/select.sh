#!/bin/bash
# тест выделения области (-s) и копирования в буфер
set -u
export DISPLAY=:99
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/sel.png
[ -f /tmp/shot.pid ] && kill "$(cat /tmp/shot.pid)" 2>/dev/null
sleep 0.3
rm -f "$OUT"
setsid python3 "$HERE/../shot.py" -s -o "$OUT" >/tmp/sel.log 2>&1 &
echo $! > /tmp/shot.pid
sleep 2

# окно выбора: любое окно с именем shot размером с экран
for W in $(xdotool search --name "^shot$"); do
  eval "$(xdotool getwindowgeometry --shell "$W")"
  echo "окно $W: ${WIDTH}x${HEIGHT}"
done
# выделяем (300,250)-(700,550)
xdotool mousemove 300 250 mousedown 1 sleep 0.1
for i in $(seq 0 10); do xdotool mousemove $((300 + i * 40)) $((250 + i * 30)); done
xdotool mouseup 1
sleep 1.5

WID=$(xdotool search --name "^shot$" | head -1)
echo "редактор: $WID"
if [ -f "$OUT" ]; then
  echo "файл появился до 'готово' — подозрительно"
fi
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "размер окна редактора ${WIDTH}x${HEIGHT}"
xdotool mousemove $((WIDTH - 45)) 22 click 1
sleep 1.2
echo "--- log:"; cat /tmp/sel.log
if [ -f "$OUT" ]; then
  python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG',pb.get_width(),'x',pb.get_height(),'(ожидание 400x300)')"
else
  echo "FAIL: файл не создан"
fi
