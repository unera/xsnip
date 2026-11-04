#!/bin/bash
# сценарий «как старый скрипт со scrot»: xsnip -s -o file && xclip из файла
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgcompat2
rm -rf "$XDG_CONFIG_HOME"
mkdir -p "$XDG_CONFIG_HOME"
pkill -x xsnip 2>/dev/null || true
pkill -x xclip 2>/dev/null || true
sleep 0.5

run() {
    local mode=$1
    rm -f /tmp/compat.png
    setsid /usr/bin/xsnip -s -o /tmp/compat.png >/tmp/compat.log 2>&1 &
    sleep 2.5
    xdotool mousemove 200 150 mousedown 1
    sleep 0.1
    for i in $(seq 0 7); do xdotool mousemove $((200 + i * 40)) $((150 + i * 25)); sleep 0.03; done
    xdotool mouseup 1
    sleep 1.5
    if [ "$mode" = "esc" ]; then
        xdotool key Escape
    else
        local WID
        WID=$(xdotool search --name "^xsnip$" | head -1)
        eval "$(xdotool getwindowgeometry --shell "$WID")"
        xdotool mousemove $((X + WIDTH - 30)) $((Y + 20)) click 1
    fi
    sleep 1.5
    local fsize=0
    [ -f /tmp/compat.png ] && fsize=$(stat -c%s /tmp/compat.png)
    echo "  выход $mode: файл ${fsize} байт"
    if [ "$fsize" -gt 0 ]; then
        (xclip -selection clipboard -t image/png -i /tmp/compat.png &)
        sleep 0.7
        local csize
        csize=$(timeout 5 xclip -selection clipboard -t image/png -o 2>/dev/null | wc -c)
        echo "    после xclip из файла: буфер ${csize} байт"
        pkill -x xclip 2>/dev/null || true
    fi
}

echo "--- как старый скрипт: xsnip -s -o file && xclip из файла"
run esc
run ok
