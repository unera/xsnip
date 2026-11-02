#!/bin/bash
# запоминание настроек: изменили цвет и размер -> следующий запуск стартует с ними
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgpersist
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

# запуск 1: зелёный цвет, «+» дважды (толщина 1.2%), текст-инструмент
setsid "$APP" -o /tmp/p1.png >/tmp/p1.log 2>&1 &
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 260)) $((Y + 22)) click 1     # зелёный
sleep 0.2
xdotool mousemove $((X + 540)) $((Y + 22)) click 1     # +
sleep 0.2
xdotool mousemove $((X + 540)) $((Y + 22)) click 1     # +
sleep 0.2
xdotool mousemove $((X + 175)) $((Y + 22)) click 1     # текст
sleep 0.3
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.2
echo "--- конфиг после 1-го запуска:"; cat "$XDG_CONFIG_HOME/xsnip/config"

# запуск 2: без опций — должен подняться зелёным, толще и в режиме текста
XSNIP_DEBUG=1 setsid "$APP" -o /tmp/p2.png >/tmp/p2.log 2>&1 &
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"
python3 "$HERE/snapshot.py" "$WID" /tmp/p2_win.png
echo "--- конфиг после 2-го запуска:"; cat "$XDG_CONFIG_HOME/xsnip/config"
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1
