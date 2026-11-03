#!/bin/bash
# Enter вне ввода текста = «готово»; в режиме ввода = поставить текст
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgent
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}
pkill -x xsnip 2>/dev/null
sleep 0.4
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

echo "=== 1. Enter с пером -> файл сохраняется и окно закрывается"
rm -f /tmp/ent1.png
setsid "$APP" -o /tmp/ent1.png >/tmp/ent1.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 6); do xdotool mousemove $((X + 200 + i * 25)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.3
xdotool key Return
sleep 1.2
[ -f /tmp/ent1.png ] && echo "  файл создан" || echo "  ПЛОХО: файла нет"
pgrep -x xsnip >/dev/null && echo "  ПЛОХО: окно живо" || echo "  окно закрылось"

echo "=== 2. Enter в режиме ввода ставит текст, окно не закрывается"
rm -f /tmp/ent2.png
setsid "$APP" -o /tmp/ent2.png >/tmp/ent2.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool key t; sleep 0.3
xdotool mousemove $((X + 150)) $((Y + 300)) click 1; sleep 0.4
xdotool type --delay 40 "ok"; sleep 0.3
xdotool key Return
sleep 0.6
pgrep -x xsnip >/dev/null && echo "  окно живо — верно" || echo "  ПЛОХО: окно закрылось на вводе текста"
python3 "$HERE/snapshot.py" "$WID" /tmp/ent2_win.png >/dev/null 2>&1
xdotool key Return
sleep 1.2
[ -f /tmp/ent2.png ] && echo "  второй Enter сохранил файл" || echo "  ПЛОХО: файла нет"
