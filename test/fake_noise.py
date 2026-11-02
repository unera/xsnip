import os
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
import cairo

W = int(os.environ.get("FW", 3000))
H = int(os.environ.get("FH", 2000))

class W_(Gtk.Window):
    def __init__(self):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self.set_decorated(False)
        self.move(0, 0)
        self.set_default_size(W, H)
        a = Gtk.DrawingArea()
        a.connect("draw", self.draw)
        self.add(a)
        self.show_all()

    def draw(self, _a, cr):
        import random
        random.seed(1)
        surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
        c = cairo.Context(surf)
        c.set_source_rgb(0.5, 0.5, 0.5)
        c.paint()
        for i in range(120000):
            x = random.randrange(W); y = random.randrange(H)
            c.set_source_rgb(random.random(), random.random(), random.random())
            c.rectangle(x, y, 3, 3)
        c.fill()
        cr.set_source_surface(surf, 0, 0)
        cr.paint()
        return False

w = W_()
w.connect("destroy", Gtk.main_quit)
Gtk.main()
