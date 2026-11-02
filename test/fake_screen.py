import os
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk

class W(Gtk.Window):
    def __init__(self):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self.set_decorated(False)
        self.move(0, 0)
        self.W = int(os.environ.get("FW", 1280)); self.H = int(os.environ.get("FH", 800))
        self.set_default_size(self.W, self.H)
        a = Gtk.DrawingArea()
        a.connect("draw", self.draw)
        self.add(a)
        self.show_all()
    def draw(self, _a, cr):
        cr.set_source_rgb(0.95, 0.95, 0.92); cr.paint()
        for i, c in enumerate([(0.2,0.4,0.8),(0.1,0.7,0.3),(0.9,0.7,0.1),(0.6,0.2,0.6)]):
            cr.set_source_rgb(*c); cr.rectangle(80+i*280, 200, 220, 160); cr.fill()
        cr.set_source_rgb(0.1,0.1,0.1); cr.set_font_size(42)
        cr.move_to(90, 120); cr.show_text("Fake screen %dx%d" % (self.W, self.H))
        return False

w = W(); w.connect("destroy", Gtk.main_quit); Gtk.main()
