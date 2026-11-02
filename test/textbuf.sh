#!/bin/bash
# тест текста и копирования в буфер (запуск без -o)
set -u
export DISPLAY=:99
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f /tmp/shot.pid ] && kill "$(cat /tmp/shot.pid)" 2>/dev/null
sleep 0.3
rm -f /tmp/clip.png
setsid python3 "$HERE/../shot.py" >/tmp/txt.log 2>&1 &
echo $! > /tmp/shot.pid
sleep 2
WID=$(xdotool search --name "^shot$" | head -1)
echo "окно $WID"
xdotool windowactivate "$WID" 2>/dev/null
xdotool key --window "$WID" 2
sleep 0.3
xdotool mousemove 300 200 sleep 0.2 click 1
sleep 0.8
xdotool type --delay 60 "Hello shot"
sleep 0.3
xdotool key Return
sleep 0.5
eval "$(xdotool getwindowgeometry --shell "$WID")"
xdotool mousemove $((WIDTH - 45)) 22 click 1
sleep 1.5
echo "--- log:"; cat /tmp/txt.log
xclip -selection clipboard -t image/png -o > /tmp/clip.png 2>/tmp/clip.err || echo "FAIL: буфер не отдал image/png: $(cat /tmp/clip.err)"
if [ -s /tmp/clip.png ]; then
  python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('/tmp/clip.png');print('буфер PNG',pb.get_width(),'x',pb.get_height())"
fi
