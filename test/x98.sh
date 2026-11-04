#!/bin/bash
# поднять Xvfb :98 (3000x2000) с шумовым «экраном»
export DISPLAY=:98
if ! xdpyinfo >/dev/null 2>&1; then
  setsid Xvfb :98 -screen 0 3000x2000x24 >/tmp/x98.log 2>&1 &
  sleep 2
fi
if ! xdotool search --name "fake_noise" >/dev/null 2>&1; then
  xdotool search --name "Fake" 2>/dev/null | head -1 >/dev/null
  FW=3000 FH=2000 setsid python3 "$(dirname "$0")/fake_noise.py" >/tmp/noise98.log 2>&1 &
  sleep 4
fi
