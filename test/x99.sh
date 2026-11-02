#!/bin/bash
# поднять Xvfb :99 с тестовым «экраном», если его ещё нет
export DISPLAY=:99
if ! xdpyinfo >/dev/null 2>&1; then
  setsid Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb99.log 2>&1 &
  sleep 1.5
fi
if ! xdotool search --name "fake_screen" >/dev/null 2>&1; then
  FW=1280 FH=800 setsid python3 "$(dirname "$0")/fake_screen.py" >/tmp/fake99.log 2>&1 &
  sleep 1.5
fi
