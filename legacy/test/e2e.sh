#!/bin/bash
# e2e-тест xsnip под Xvfb: запуск, рисование мышью, сохранение по кнопке "готово".
set -u
export DISPLAY=:99
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip.py"
OUT=/tmp/out.png

[ -f /tmp/xsnip.pid ] && kill "$(cat /tmp/xsnip.pid)" 2>/dev/null
sleep 0.3
rm -f "$OUT"

setsid python3 "$APP" -o "$OUT" >/tmp/xsnip.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2

WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -z "$WID" ]; then echo "FAIL: окно редактора не найдено"; cat /tmp/xsnip.log; exit 1; fi
xdotool getwindowgeometry "$WID"

# рисуем штрих по канвасу
xdotool mousemove 200 300 mousedown 1
for i in $(seq 0 20); do xdotool mousemove $((200 + i * 20)) $((300 + i * 10)); done
xdotool mouseup 1
sleep 0.4

eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "window ${WIDTH}x${HEIGHT}"
# кнопка "готово" — правый край панели
xdotool mousemove $((WIDTH - 45)) 22 click 1
sleep 1.2

echo "--- stderr:"; cat /tmp/xsnip.log
if [ -f "$OUT" ]; then
  ls -l "$OUT"
  python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG OK', pb.get_width(), pb.get_height(), 'alpha' if pb.get_has_alpha() else 'no-alpha')"
else
  echo "FAIL: файл не создан"
  exit 1
fi
