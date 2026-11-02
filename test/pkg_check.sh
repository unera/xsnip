#!/bin/bash
# проверка установленного из пакета /usr/bin/xsnip под Xvfb: выделение + штрих + файл
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgpkg
HERE=$(cd "$(dirname "$0")" && pwd)
APP=/usr/bin/xsnip
OUT=/tmp/pkg_shot.png

pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
rm -f "$OUT"

setsid "$APP" -s -o "$OUT" >/tmp/pkg.log 2>&1 &
sleep 2.5
SEL=$(xdotool search --name "^xsnip-select$" | head -1)
if [ -z "$SEL" ]; then echo "FAIL: окна выделения нет"; cat /tmp/pkg.log; exit 1; fi
xdotool mousemove 250 200 mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((250 + i * 40)) $((200 + i * 30)); done
xdotool mouseup 1
sleep 2
WID=$(xdotool search --name "^xsnip$" | head -1)
if [ -z "$WID" ]; then echo "FAIL: редактор не открылся"; cat /tmp/pkg.log; exit 1; fi
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "редактор ${WIDTH}x${HEIGHT} at ${X},${Y}"
# штрих
xdotool mousemove $((X + 100)) $((Y + 120)) mousedown 1
for i in $(seq 0 10); do xdotool mousemove $((X + 100 + i * 25)) $((Y + 120 + i * 8)); done
xdotool mouseup 1
sleep 0.3
# текст
xdotool mousemove $((X + 175)) $((Y + 22)) click 1
sleep 0.3
xdotool mousemove $((X + 80)) $((Y + 250)) click 1
sleep 0.6
xdotool type --delay 60 "из пакета"
sleep 0.3
xdotool key Return
sleep 0.5
python3 "$HERE/snapshot.py" "$WID" /tmp/pkg_win.png
xdotool mousemove $((X + WIDTH - 40)) $((Y + 22)) click 1
sleep 1.5
echo "--- лог:"; cat /tmp/pkg.log
if [ -f "$OUT" ]; then
  python3 -c "import gi;gi.require_version('GdkPixbuf','2.0');from gi.repository import GdkPixbuf;pb=GdkPixbuf.Pixbuf.new_from_file('$OUT');print('PNG',pb.get_width(),'x',pb.get_height(),'(ожидание 400x300)')"
else
  echo "FAIL: файла нет"
fi
