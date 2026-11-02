#!/bin/bash
# попытка поднять в Xvfb два Xinerama-экрана и посмотреть, что видит xsnip
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
pkill -x Xvfb 2>/dev/null || true
sleep 0.5

try() {
    local port=$1; shift
    Xvfb ":$port" "$@" >/tmp/xvfb_multi.log 2>&1 &
    sleep 2
    echo "--- запуск с ключами: $*"
    DISPLAY=":$port" xdpyinfo -ext XINERAMA 2>/dev/null | sed -n '/XINERAMA/,/^[a-z]/p' | head -8
    DISPLAY=":$port" xdpyinfo 2>/dev/null | grep -E "dimensions|number of screens"
    pkill -x Xvfb 2>/dev/null || true
    sleep 0.5
}

try 96 -screen 0 1920x1080x24 -screen 1 1920x1200x24 +xinerama
try 95 -screen 0 3840x2160x24 +xinerama
