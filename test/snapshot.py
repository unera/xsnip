#!/usr/bin/env python3
"""Сохранить снимок чужого X-окна по XID (для проверки UI в тестах)."""
import sys
import gi
gi.require_version("Gdk", "3.0")
gi.require_version("GdkX11", "3.0")
from gi.repository import Gdk, GdkX11

xid, out = int(sys.argv[1]), sys.argv[2]
disp = Gdk.Display.get_default()
gwin = GdkX11.X11Window.foreign_new_for_display(disp, xid)
if gwin is None:
    sys.exit("нет окна с id %d" % xid)
win = gwin.get_width(), gwin.get_height()
pb = Gdk.pixbuf_get_from_window(gwin, 0, 0, win[0], win[1])
pb.savev(out, "png", [], [])
print("%s %dx%d" % (out, win[0], win[1]))
