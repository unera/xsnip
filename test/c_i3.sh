#!/bin/bash
# проверка под настоящим i3: диалоговое окно (floating + позиция) и обычное (тайл)
set -u
export DISPLAY=:97
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../xsnip"
CFG=/tmp/cfgi3

pkill -x xsnip 2>/dev/null
pkill -f "Xvfb :97" 2>/dev/null
pkill -f "i3 -c" 2>/dev/null
sleep 0.6

Xvfb :97 -screen 0 1280x800x24 >/tmp/xvfb97.log 2>&1 &
sleep 1.5
cat > /tmp/i3cfg <<'EOF'
font pango:DejaVu Sans 8
# без панели
EOF
setsid i3 -c /tmp/i3cfg >/tmp/i3.log 2>&1 &
sleep 2.5
echo "--- i3 запущен:"; i3-msg -t get_version 2>/dev/null | head -c 120; echo

rm -rf "$CFG"; mkdir -p "$CFG"
export XDG_CONFIG_HOME="$CFG"

echo "=== dialog + pos tl"
setsid "$APP" -o /tmp/i3_dialog.png --window dialog --pos tl >/tmp/i3_d.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
echo "окно: $(xdotool getwindowgeometry "$WID" | tr '\n' ' ')"
i3-msg -t get_tree 2>/dev/null | python3 -c '
import json,sys
t=json.load(sys.stdin)
def walk(n):
    if n.get("window") is not None and n.get("name")=="xsnip":
        print("  i3: floating=%s window_type=%s rect=%s" % (n.get("floating"), n.get("window_type"), n.get("rect")))
    for c in n.get("nodes",[])+n.get("floating_nodes",[]):
        walk(c)
walk(t)
'
pkill -x xsnip 2>/dev/null
sleep 0.8

echo "=== normal"
setsid "$APP" -o /tmp/i3_normal.png --window normal >/tmp/i3_n.log 2>&1 &
sleep 2.5
WID=$(xdotool search --name "^xsnip$" | head -1)
echo "окно: $(xdotool getwindowgeometry "$WID" | tr '\n' ' ')"
i3-msg -t get_tree 2>/dev/null | python3 -c '
import json,sys
t=json.load(sys.stdin)
def walk(n):
    if n.get("window") is not None and n.get("name")=="xsnip":
        print("  i3: floating=%s rect=%s" % (n.get("floating"), n.get("rect")))
    for c in n.get("nodes",[])+n.get("floating_nodes",[]):
        walk(c)
walk(t)
'
echo "--- логи:"; head -3 /tmp/i3_d.log; head -3 /tmp/i3_n.log
pkill -x xsnip 2>/dev/null
