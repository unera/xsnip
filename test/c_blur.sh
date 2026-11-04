#!/bin/bash
# --close-on-blur: окно закрывается при потере фокуса, работа доделывается
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgblur
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}

alive() { xwininfo -id "$1" >/dev/null 2>&1; }

rm -rf "$XDG_CONFIG_HOME"
mkdir -p "$XDG_CONFIG_HOME"
pkill -x xsnip 2>/dev/null || true
sleep 0.5

echo "=== 1. без флага: фокус уходит, окно остаётся"
rm -f /tmp/blur1.png
setsid "$APP" -o /tmp/blur1.png >/tmp/blur1.log 2>&1 &
sleep 2.5
A=$(xdotool search --name "^xsnip$" | head -1)
setsid "$APP" -o /tmp/blur_other.png >/dev/null 2>&1 &
sleep 2.5
if alive "$A"; then echo "  окно $A живо — верно"; else echo "  ПЛОХО: закрылось без флага"; fi
pkill -x xsnip 2>/dev/null
sleep 0.7

echo "=== 2. с флагом: фокус уходит -> окно закрывается, файл записан, буфер на месте"
rm -f /tmp/blur2.png
setsid "$APP" --close-on-blur -o /tmp/blur2.png >/tmp/blur2.log 2>&1 &
sleep 2.5
A=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$A")"
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 6); do xdotool mousemove $((X + 200 + i * 25)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.5
setsid "$APP" -o /tmp/blur_other2.png >/dev/null 2>&1 &
sleep 2.5
if alive "$A"; then echo "  ПЛОХО: окно $A не закрылось"; else echo "  окно $A закрылось — верно"; fi
if [ -f /tmp/blur2.png ]; then echo "  файл записан: $(stat -c%s /tmp/blur2.png) байт"; else echo "  ПЛОХО: файла нет"; fi
size=$(timeout 5 xclip -selection clipboard -t image/png -o 2>/dev/null | wc -c)
echo "  буфер: ${size} байт"

echo "=== 3. настройка запомнилась:"
grep '^blur=' "$XDG_CONFIG_HOME/xsnip/config" 2>/dev/null || echo "  нет строки blur"
pkill -x xsnip 2>/dev/null
