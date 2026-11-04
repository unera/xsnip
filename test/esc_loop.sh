#!/bin/bash
# 6 подряд сценариев «снимок -> Esc» и проверка буфера каждый раз
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgloop
pkill -x xsnip 2>/dev/null || true
sleep 0.5
ok=0; bad=0
for i in $(seq 1 6); do
  rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
  setsid /home/dsh/work/ai/xsnip/xsnip -s >/tmp/loop$i.log 2>&1 &
  sleep 2.2
  xdotool mousemove 200 150 mousedown 1
  sleep 0.1
  for k in $(seq 0 6); do xdotool mousemove $((200+k*40)) $((150+k*25)); sleep 0.03; done
  xdotool mouseup 1
  sleep 1.5
  xdotool key Escape
  sleep 0.8
  size=$(timeout 5 xclip -selection clipboard -t image/png -o 2>/dev/null | wc -c)
  if [ "$size" -gt 100 ]; then echo "  попытка $i: буфер ${size} байт — ок"; ok=$((ok+1)); else echo "  попытка $i: ПУСТО"; bad=$((bad+1)); fi
done
echo "итог: ок $ok, пусто $bad"
