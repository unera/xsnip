#!/bin/bash
# шорткаты при русской раскладке: p/t/r и Ctrl+Z должны работать
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfglayout
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}

pkill -x xsnip 2>/dev/null
sleep 0.4
setxkbmap -layout us 2>/dev/null
old_layout=$(setxkbmap -query 2>/dev/null | awk '/^layout/{print $2}')
echo "раскладка до теста: ${old_layout}"
setxkbmap -layout ru 2>/dev/null
echo "раскладка в тесте: $(setxkbmap -query 2>/dev/null | awk '/^layout/{print $2}')"

rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
setsid "$APP" -o /tmp/layout.png >/tmp/layout.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/layout.log; exit 1; }
cfg() { grep '^tool=' "$XDG_CONFIG_HOME/xsnip/config" 2>/dev/null; }

# xdotool key p нажимает физическую клавишу 'p'; клиент при ru-раскладке
# получит keysym Cyrillic_pe — раньше из-за этого шорткат не срабатывал
for k in t r p; do
  xdotool key "$k"; sleep 0.3
  printf 'ru + %-2s -> %s\n' "$k" "$(cfg)"
done

# Ctrl+Z при русской раскладке (физическая z = Cyrillic_ya)
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + 300)) $((Y + 300)) mousedown 1
sleep 0.15
for i in $(seq 0 6); do xdotool mousemove $((X + 300 + i * 20)) $((Y + 300)); sleep 0.03; done
xdotool mouseup 1
sleep 0.4
xdotool key ctrl+z
sleep 0.5
python3 "$HERE/snapshot.py" "$WID" /tmp/layout_after_undo.png
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/layout_after_undo.png')
w,h,s,nc = pb.get_width(),pb.get_height(),pb.get_rowstride(),pb.get_n_channels()
d = pb.get_pixels(); n = 0
for y in range(60, h):
    row=d[y*s:(y+1)*s]
    for x in range(w):
        i=x*nc
        if row[i]>150 and row[i+1]<120 and row[i+2]<120:
            n += 1
print("после Ctrl+Z на ru-раскладке красных пикселей:", n, "(0 = штрих отменён)")
PY

setxkbmap -layout us 2>/dev/null
pkill -x xsnip 2>/dev/null
