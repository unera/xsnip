#!/bin/bash
# подсказки и языки: hover над кнопкой -> окно xsnip-tip; снимки панели на en/ru/zh
set -u
export DISPLAY=:99
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../xsnip}

pkill -x xsnip 2>/dev/null
sleep 0.5

run_lang() {
    local lang=$1
    export XSNIP_LANG="$lang"
    export XDG_CONFIG_HOME="/tmp/cfgtip_$lang"
    rm -rf "$XDG_CONFIG_HOME"; mkdir -p "$XDG_CONFIG_HOME"
    setsid "$APP" -o "/tmp/tip_$lang.png" >"/tmp/tip_$lang.log" 2>&1 &
    sleep 2.5
    local WID
    WID=$(xdotool search --name "^xsnip$" | head -1)
    [ -z "$WID" ] && { echo "FAIL: нет окна для $lang"; cat "/tmp/tip_$lang.log"; return 1; }
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    xdotool mousemove $((X + 25)) $((Y + 22))
    sleep 1.2
    local TIP
    TIP=$(xdotool search --name "^xsnip-tip$" | head -1)
    if [ -n "$TIP" ]; then
        python3 "$HERE/snapshot.py" "$TIP" "/tmp/tipbox_$lang.png" >/dev/null
        echo "  $lang: подсказка показана"
        rc=0
    else
        echo "  $lang: подсказки нет (плохо)"
        rc=1
    fi
    python3 "$HERE/snapshot.py" "$WID" "/tmp/tipwin_$lang.png" >/dev/null
    pkill -x xsnip 2>/dev/null
    sleep 0.5
}

for l in en ru zh; do run_lang "$l"; done

python3 - <<'PY'
import gi
gi.require_version('GdkPixbuf','2.0')
from gi.repository import GdkPixbuf
for l in ('en','ru','zh'):
    pb = GdkPixbuf.Pixbuf.new_from_file('/tmp/tipwin_%s.png' % l)
    pb.new_subpixbuf(0, 0, 420, 45).savev('/tmp/panel_%s.png' % l, 'png', [], [])
print("панели сохранены")
PY
exit $rc
