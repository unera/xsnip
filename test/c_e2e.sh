#!/bin/bash
# e2e-тест C-версии под Xvfb: запуск, штрих мышью, сохранение по «готово»
set -u
export DISPLAY=:99
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/c_out.png
XDG_CONFIG_HOME=/tmp/cfgtest
export XDG_CONFIG_HOME
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -f "$OUT"

setsid "$APP" -o "$OUT" >/tmp/c_shot.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2
if ! kill -0 "$(cat /tmp/xsnip.pid)" 2>/dev/null; then echo "FAIL: процесс умер"; cat /tmp/c_shot.log; exit 1; fi

WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -z "$WID" ]; then echo "FAIL: окна нет"; cat /tmp/c_shot.log; exit 1; fi
xdotool getwindowgeometry "$WID"

# штрих
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
for i in $(seq 0 20); do xdotool mousemove $((X + 200 + i * 20)) $((Y + 300 + i * 10)); done
xdotool mouseup 1
sleep 0.4

echo "window ${WIDTH}x${HEIGHT}"
xdotool mousemove $((X + WIDTH - 45)) $((Y + 22)) click 1
sleep 1.2

echo "--- log:"; cat /tmp/c_shot.log
if [ -f "$OUT" ]; then
  ls -l "$OUT"
else
  echo "FAIL: файл не создан"
  exit 1
fi
