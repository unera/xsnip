#!/bin/bash
# сразу после Esc (без задержки) проверяем буфер
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgracе
rm -rf /tmp/cfgracе; mkdir -p /tmp/cfgracе
pkill -x xsnip 2>/dev/null || true
sleep 0.5
rm -f /tmp/xsnip-clip.log
XSNIP_DEBUG=1 setsid /home/dsh/work/ai/xsnip/xsnip -s >/tmp/race.log 2>&1 &
sleep 2.5
xdotool mousemove 200 150 mousedown 1; sleep 0.1
for i in $(seq 0 8); do xdotool mousemove $((200+i*40)) $((150+i*25)); sleep 0.03; done
xdotool mouseup 1
sleep 2
xdotool key Escape
for d in 0 0.05 0.2 1; do
  sleep "$d"
  size=$(timeout 5 xclip -selection clipboard -t image/png -o 2>/dev/null | wc -c)
  echo "  через ${d}s после Esc: буфер ${size} байт"
done
echo "--- лог демона:"; tail -6 /tmp/xsnip-clip.log 2>/dev/null
