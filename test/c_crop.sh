#!/bin/bash
# режим «вырезать»: рамка на картинке -> обрезка изображения
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgcrop
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/crop.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

XSNIP_DEBUG=1 setsid "$APP" -o "$OUT" >/tmp/crop.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/crop.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "окно ${WIDTH}x${HEIGHT}"

# кнопка «вырезать» (id=5) по отладочному дампу
CROP_X=$(grep -o "btn id=5 x=[0-9]*" /tmp/crop.log | tail -1 | cut -d= -f3)
CROP_W=$(grep -o "btn id=5 x=[0-9]* w=[0-9]*" /tmp/crop.log | tail -1 | rev | cut -d= -f1 | rev)
echo "кнопка вырезать: x=${CROP_X} w=${CROP_W}"
xdotool mousemove $((X + CROP_X + CROP_W / 2)) $((Y + 22)) click 1
sleep 0.4

xdotool mousemove $((X + 200)) $((Y + 200)) mousedown 1
sleep 0.2
for i in $(seq 0 10); do xdotool mousemove $((X + 200 + i * 30)) $((Y + 200 + i * 20)); sleep 0.03; done
sleep 0.3
python3 "$HERE/snapshot.py" "$WID" /tmp/crop_preview.png
xdotool mouseup 1
sleep 1

python3 "$HERE/snapshot.py" "$WID" /tmp/crop_after.png
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.5
grep -E "commit|btn id=8" /tmp/crop.log | tail -2
python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/crop.png')
print("PNG после обрезки: %dx%d (ожидание около 379x253)" % (pb.get_width(), pb.get_height()))
PY
