#!/bin/bash
# буфер всегда актуален: сразу после открытия, после штриха, после undo, после Esc
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgauto
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}
pkill -x xsnip 2>/dev/null
sleep 0.4
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"

grab() {  # забрать PNG из буфера и сказать, есть ли красное
  timeout 15 xclip -selection clipboard -t image/png -o > /tmp/auto.png 2>/dev/null || { echo "  буфер пуст"; return 1; }
  python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/auto.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels(); n = 0
for y in range(0, h, 2):
    row=d[y*s:(y+1)*s]
    for x in range(0, w, 2):
        i=x*nc
        if row[i]>150 and row[i+1]<120 and row[i+2]<120: n += 1
print("  буфер %dx%d, красных пикселей: %d" % (w,h,n))
PY
}

setsid "$APP" -o /tmp/auto.png >/tmp/auto.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "нет окна"; cat /tmp/auto.log; exit 1; }

echo "=== 1. сразу после открытия (ничего не нажимали)"
grab

echo "=== 2. после штриха"
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 200)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 6); do xdotool mousemove $((X + 200 + i * 25)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.8
grab

echo "=== 3. после undo"
xdotool key ctrl+z
sleep 0.8
grab

echo "=== 4. после Esc (окно закрыто, буфер должен остаться)"
xdotool key Escape
sleep 1
grab

echo "=== 5. кнопок без подписи осталось (undo + ok = 2 иконки):"
grep -c "btn id=" /tmp/auto.log 2>/dev/null || true
