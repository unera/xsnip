#!/bin/bash
# «клик-клик»: первый клик угол, второй — противоположный, затем обрезка
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgclick
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/clickcrop.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -o "$OUT" >/tmp/clickcrop.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
eval "$(xdotool getwindowgeometry --shell "$WID")"

xdotool key 3                    # вырезать
sleep 0.3
xdotool mousemove $((X + 300)) $((Y + 250)) click 1     # первый угол
sleep 0.4
xdotool mousemove $((X + 600)) $((Y + 450))             # показать рамку
sleep 0.3
python3 "$HERE/snapshot.py" "$WID" /tmp/clickcrop_preview.png
xdotool click 1                                          # второй угол -> обрезка
sleep 0.8
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((X + WIDTH - 25)) $((Y + 22)) click 1
sleep 1.2
python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG %dx%d (ожидание 378x252)' % (pb.get_width(), pb.get_height()))"
