#!/bin/bash
# тест буфера (копирование при закрытии без -o) и настроек между запусками
set -u
export DISPLAY=:99
XDG_CONFIG_HOME=/tmp/cfgtest
export XDG_CONFIG_HOME
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"

pkill -f "xclip" 2>/dev/null
pkill -x xsnip 2>/dev/null
sleep 0.5
rm -f /tmp/c_clip.png /tmp/cfgtest/xsnip/config

setsid "$APP" >/tmp/c_clip.log 2>&1 &
echo $! > /tmp/xsnip.pid
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/c_clip.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"

# сменим цвет на 2-й (зелёный): кнопка цвета — ищем по позиции после «текст»
xdotool mousemove $((X + 200)) $((Y + 22)) click 1   # примерно 2-й свотч
sleep 0.3
# штрих
xdotool mousemove $((X + 300)) $((Y + 300)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X + 300 + i * 25)) $((Y + 300 + i * 5)); done
xdotool mouseup 1
sleep 0.3
xdotool mousemove $((X + WIDTH - 45)) $((Y + 22)) click 1   # готово -> буфер
sleep 1.5

echo "--- процессы xsnip:"; pgrep -af "/xsnip$" | head -3
timeout 5 xclip -selection clipboard -t image/png -o > /tmp/c_clip.png 2>/tmp/c_clip.err \
  && echo "буфер забран" || { echo "FAIL буфер: $(cat /tmp/c_clip.err)"; exit 1; }
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/c_clip.png')
w,h,stride,nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
d = pb.get_pixels()
print("буфер PNG %dx%d" % (w,h))
# в окне 1216x680 картинка 1280x800 -> масштаб 0.85; штрих примерно там же
cols = {}
for y in range(0, h, 3):
    row = d[y*stride:(y+1)*stride]
    for x in range(0, w, 3):
        i = x*nc
        c = (row[i], row[i+1], row[i+2])
        cols[c] = cols.get(c, 0) + 1
top = sorted(cols.items(), key=lambda kv: -kv[1])[:4]
print("частые цвета:", [("#%02x%02x%02x" % c, n) for c, n in top])
PY
echo "--- конфиг:"; cat /tmp/cfgtest/xsnip/config 2>/dev/null || echo "конфига нет"
