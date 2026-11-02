#!/bin/bash
# INCR-тест: большой PNG (не влезает в одно X-свойство) через буфер
set -u
export DISPLAY=:98
export XDG_CONFIG_HOME=/tmp/cfginсr
export XDG_CONFIG_HOME=/tmp/cfginr
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"

pkill -x xsnip 2>/dev/null
pkill -f "Xvfb :98" 2>/dev/null
sleep 0.5
Xvfb :98 -screen 0 3000x2000x24 >/tmp/xvfb98.log 2>&1 &
sleep 1.5
FW=3000 FH=2000 setsid python3 "$HERE/fake_noise.py" >/tmp/noise.log 2>&1 &
sleep 4

rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
pkill -f "xclip -selection" 2>/dev/null
setsid "$APP" >/tmp/c_incr.log 2>&1 &
sleep 3
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/c_incr.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "редактор ${WIDTH}x${HEIGHT}"
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 2.5
echo "--- процессы xsnip:"; pgrep -ax xsnip | head -3
echo "--- забираем буфер:"
timeout 30 xclip -selection clipboard -t image/png -o > /tmp/big_clip.png 2>/tmp/big_clip.err \
  && echo "OK забрано" || { echo "FAIL: $(cat /tmp/big_clip.err)"; exit 1; }
ls -l /tmp/big_clip.png
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/big_clip.png')
print("PNG целый:", pb.get_width(), "x", pb.get_height())
PY
