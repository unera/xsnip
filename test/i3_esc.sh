#!/bin/bash
# воспроизведение «Esc -> пустой буфер» под i3
set -u
HERE=/home/dsh/work/ai/xsnip
pkill -x xsnip 2>/dev/null || true
pkill -f "Xvfb :97" 2>/dev/null || true
pkill -f "i3 -c /tmp/i3cfg" 2>/dev/null || true
sleep 0.8
export DISPLAY=:97
Xvfb :97 -screen 0 1280x800x24 >/tmp/x97.log 2>&1 &
sleep 2
printf 'font pango:DejaVu Sans 8\n' > /tmp/i3cfg
setsid i3 -c /tmp/i3cfg >/tmp/i3.log 2>&1 &
sleep 2.5
export XDG_CONFIG_HOME=/tmp/cfgi3esc
rm -rf "$XDG_CONFIG_HOME"
mkdir -p "$XDG_CONFIG_HOME"

run_case() {
    local mode=$1
    pkill -x xsnip 2>/dev/null || true
    sleep 0.5
    setsid "$HERE/xsnip" -s >"/tmp/i3_$mode.log" 2>&1 &
    sleep 2.5
    xdotool mousemove 200 150 mousedown 1
    sleep 0.1
    for i in $(seq 0 8); do xdotool mousemove $((200 + i * 40)) $((150 + i * 25)); sleep 0.03; done
    xdotool mouseup 1
    sleep 2
    local WID
    WID=$(xdotool search --name "^xsnip$" | head -1)
    if [ -z "$WID" ]; then echo "  $mode: окна редактора нет"; return; fi
    xdotool windowactivate "$WID" 2>/dev/null || true
    sleep 0.4
    if [ "$mode" = "ok" ]; then
        eval "$(xdotool getwindowgeometry --shell "$WID")"
        xdotool mousemove $((X + WIDTH - 30)) $((Y + 20)) click 1
    else
        xdotool key Escape
    fi
    sleep 1.5
    local size
    size=$(timeout 10 xclip -selection clipboard -t image/png -o 2>/dev/null | wc -c)
    echo "  выход $mode: буфер ${size} байт, процессов xsnip: $(pgrep -c -x xsnip || echo 0)"
}

echo "--- i3, выход по Esc и по кнопке:"
run_case esc
run_case ok
