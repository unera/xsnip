#!/bin/bash
# тест выбора области (-s) в C-версии
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgsel
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
OUT=/tmp/c_sel.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -s -o "$OUT" >/tmp/c_sel.log 2>&1 &
sleep 2

WIN=$(xdotool search --name "^xsnip-select$" | head -1)
if [ -z "$WIN" ]; then
  echo "FAIL: окна выделения нет"; xwininfo -root -children | grep -E "xsnip" | head -5; cat /tmp/c_sel.log; exit 1
fi
eval "$(xdotool getwindowgeometry --shell "$WIN")"
echo "окно выделения ${WIDTH}x${HEIGHT}"

xdotool mousemove 300 250 mousedown 1 sleep 0.1
for i in $(seq 0 10); do xdotool mousemove $((300 + i * 40)) $((250 + i * 30)); done
sleep 0.3
python3 "$HERE/snapshot.py" "$WIN" /tmp/c_sel_frame.png
xdotool mouseup 1
sleep 1.5

WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -z "$WID" ]; then echo "FAIL: редактор не открылся"; cat /tmp/c_sel.log; exit 1; fi
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "редактор ${WIDTH}x${HEIGHT} at ${X},${Y}"
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.2
echo "--- лог:"; cat /tmp/c_sel.log
if [ -f "$OUT" ]; then
  python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG',pb.get_width(),'x',pb.get_height(),'(ожидание 400x300)')"
else
  echo "FAIL: файла нет"
fi
