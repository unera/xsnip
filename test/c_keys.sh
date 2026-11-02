#!/bin/bash
# шорткаты p/t/r переключают инструменты
set -u
export DISPLAY=:99
export XDG_CONFIG_HOME=/tmp/cfgkeys
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}
pkill -x xsnip 2>/dev/null
sleep 0.5
rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
setsid "$APP" -o /tmp/keys.png >/tmp/keys.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
[ -z "$WID" ] && { echo "FAIL: окна нет"; cat /tmp/keys.log; exit 1; }
cfg() { grep '^tool=' "$XDG_CONFIG_HOME/xsnip/config" 2>/dev/null; }
for k in t r p; do
  xdotool key "$k"; sleep 0.3
  printf 'после %-2s: %s\n' "$k" "$(cfg)"
done
xdotool key Escape
sleep 0.5
