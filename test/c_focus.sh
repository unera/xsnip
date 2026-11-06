#!/bin/bash
# после отмены выделения фокус должен вернуться прежнему окну
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export DISPLAY=:97
export XDG_CONFIG_HOME=/tmp/cfgfocus
APP=${APP:-$HERE/../xsnip}

pkill -x xsnip 2>/dev/null || true
pkill -f "Xvfb :97" 2>/dev/null || true
pkill -f "i3 -c /tmp/i3cfg" 2>/dev/null || true
sleep 0.8

Xvfb :97 -screen 0 1280x800x24 >/tmp/x97.log 2>&1 &
sleep 2
printf 'font pango:DejaVu Sans 8\n' > /tmp/i3cfg
setsid i3 -c /tmp/i3cfg >/tmp/i3.log 2>&1 &
sleep 2.5
setsid python3 "$HERE/fake_screen.py" >/tmp/fake97.log 2>&1 &
sleep 2.5

OTHER=$(xdotool search --name "fake_screen" | head -1)
echo "постороннее окно: $OTHER"
xdotool windowactivate "$OTHER" 2>/dev/null || xdotool windowfocus "$OTHER"
sleep 0.5
BEFORE=$(xdotool getwindowfocus)
echo "фокус до запуска: $BEFORE"

rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
setsid "$APP" -s >/tmp/focus.log 2>&1 &
sleep 2.5
W=$(xdotool search --name "^xsnip-select$" | head -1)
echo "окно выбора: ${W:-НЕТ}, фокус при нём: $(xdotool getwindowfocus)"
xdotool key Escape
sleep 1.2
FOCUS_AFTER=$(xdotool getwindowfocus)
echo "фокус после Esc: $FOCUS_AFTER (был $BEFORE)"
if [ "$FOCUS_AFTER" = "$BEFORE" ]; then echo "  фокус вернулся — верно"; else echo "  ПЛОХО: фокус не вернулся"; fi

echo "=== и после успешного снимка:"
xdotool windowactivate "$OTHER" 2>/dev/null || xdotool windowfocus "$OTHER"
sleep 0.5
setsid "$APP" -s -o /tmp/focus.png >/tmp/focus2.log 2>&1 &
sleep 2.5
xdotool mousemove 200 150 mousedown 1
sleep 0.1
for i in $(seq 0 6); do xdotool mousemove $((200 + i * 30)) $((150 + i * 20)); sleep 0.03; done
xdotool mouseup 1
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -n "$WID" ]; then
    echo "  редактор открылся: $WID, фокус: $(xdotool getwindowfocus)"
    xdotool key Escape
    sleep 1
fi
pkill -x xsnip 2>/dev/null
