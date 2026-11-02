#!/usr/bin/env python3
# shot — замена scrot с мини-редактором (перо + текст).
# X11 / GTK3. Зависимости: python3-gi, gir1.2-gtk-3.0, python3-cairo.
#
#   shot -s -o /tmp/a.png     # выбрать область мышью, отредактировать, сохранить
#   shot -s                   # выбрать область, отредактировать, в буфер
#   shot -o /tmp/a.png        # весь экран сразу в редактор
#   shot                       # весь экран -> редактор -> буфер
#
# Размеры задаются в процентах от min(ширина, высота) картинки:
#   перо: 0.1 .. 5 %, шаг 0.1, по умолчанию 1
#   текст: 1 .. 50 %, шаг 0.5, по умолчанию 10
# Колесо мыши над картинкой меняет размер активного инструмента.

import argparse
import io
import math
import os
import shutil
import subprocess
import sys
import time

import cairo
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Pango", "1.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Gdk, GdkPixbuf, GLib, Gtk, Pango, PangoCairo  # noqa: E402

PEN_MIN, PEN_MAX, PEN_STEP, PEN_DEFAULT = 0.1, 5.0, 0.1, 1.0
TXT_MIN, TXT_MAX, TXT_STEP, TXT_DEFAULT = 1.0, 50.0, 0.5, 10.0
INK = (0.93, 0.16, 0.16, 1.0)  # красный


def pen_px(pct, basis):
    return max(1.0, pct / 100.0 * basis)


def txt_px(pct, basis):
    return max(1.0, pct / 100.0 * basis)


def grab_screen():
    root = Gdk.get_default_root_window()
    if root is None:
        raise RuntimeError("нет X11-экрана (нет $DISPLAY?)")
    pb = Gdk.pixbuf_get_from_window(root, 0, 0, root.get_width(), root.get_height())
    if pb is None:
        raise RuntimeError("не удалось снять экран")
    return pb


def surface_from_pixbuf(pb):
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, pb.get_width(), pb.get_height())
    cr = cairo.Context(surf)
    Gdk.cairo_set_source_pixbuf(cr, pb, 0, 0)
    cr.paint()
    return surf


def crop_pixbuf(pb, x, y, w, h):
    x = max(0, min(x, pb.get_width() - 1))
    y = max(0, min(y, pb.get_height() - 1))
    w = max(1, min(w, pb.get_width() - x))
    h = max(1, min(h, pb.get_height() - y))
    return GdkPixbuf.Pixbuf.new_subpixbuf(pb, x, y, w, h).copy()


# ---------------------------------------------------------------- отрисовка

def draw_stroke(cr, pts, width, color=INK):
    cr.set_source_rgba(*color)
    cr.set_line_width(width)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_join(cairo.LINE_JOIN_ROUND)
    if len(pts) == 1:
        x, y = pts[0]
        cr.arc(x, y, width / 2.0, 0.0, 2 * math.pi)
        cr.fill()
        return
    cr.move_to(*pts[0])
    for p in pts[1:]:
        cr.line_to(*p)
    cr.stroke()


def draw_text(cr, x, y, text, size_px, color=INK):
    layout = PangoCairo.create_layout(cr)
    fd = Pango.FontDescription("Sans")
    fd.set_absolute_size(size_px * Pango.SCALE)
    layout.set_font_description(fd)
    layout.set_text(text, -1)
    cr.set_source_rgba(*color)
    cr.move_to(x, y)
    PangoCairo.show_layout(cr, layout)


def draw_ops(cr, ops, basis):
    for op in ops:
        if op["kind"] == "stroke":
            draw_stroke(cr, op["pts"], op["w"])
        elif op["kind"] == "text":
            draw_text(cr, op["x"], op["y"], op["text"], op["size"])


# ------------------------------------------------------- выбор области экрана

class Selector(Gtk.Window):
    """Полноэкранный выбор прямоугольника по уже снятому снимку."""

    def __init__(self, shot_pb):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self.pb = shot_pb
        self.start = None
        self.cur = None
        self.rect = None
        self.cancelled = True

        self.set_decorated(False)
        self.set_resizable(False)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_keep_above(True)
        self.set_type_hint(Gdk.WindowTypeHint.DIALOG)
        self.move(0, 0)
        self.set_default_size(shot_pb.get_width(), shot_pb.get_height())

        self.area = Gtk.DrawingArea()
        self.area.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK
            | Gdk.EventMask.BUTTON_RELEASE_MASK
            | Gdk.EventMask.POINTER_MOTION_MASK
        )
        self.area.connect("draw", self.on_draw)
        self.area.connect("button-press-event", self.on_press)
        self.area.connect("motion-notify-event", self.on_motion)
        self.area.connect("button-release-event", self.on_release)
        self.add(self.area)
        self.connect("key-press-event", self.on_key)
        self.show_all()
        self.fullscreen()
        # страховка на случай, если WM не развернул окно на весь экран
        GLib.timeout_add(60, self.ensure_cover)

    def ensure_cover(self):
        win = self.get_window()
        root = Gdk.get_default_root_window()
        if win is None or root is None:
            return False
        rw, rh = root.get_width(), root.get_height()
        if win.get_width() < rw or win.get_height() < rh:
            self.unfullscreen()
            self.move(0, 0)
            self.resize(rw, rh)
        return False

    def on_key(self, _w, ev):
        if ev.keyval == Gdk.KEY_Escape:
            self.cancelled = True
            Gtk.main_quit()
            return True
        return False

    def on_press(self, _w, ev):
        self.start = (int(ev.x), int(ev.y))
        self.cur = self.start
        return True

    def on_motion(self, _w, ev):
        if self.start is None:
            return False
        self.cur = (int(ev.x), int(ev.y))
        self.queue_draw()
        return True

    def on_release(self, _w, ev):
        if self.start is None:
            return False
        self.cur = (int(ev.x), int(ev.y))
        x0, y0 = self.start
        x1, y1 = self.cur
        x, y = min(x0, x1), min(y0, y1)
        w, h = abs(x1 - x0), abs(y1 - y0)
        if w < 5 or h < 5:
            self.cancelled = True
            Gtk.main_quit()
            return True
        self.rect = (x, y, w, h)
        self.cancelled = False
        Gtk.main_quit()
        return True

    def on_draw(self, area, cr):
        cr.set_source_rgb(0, 0, 0)
        cr.paint()
        Gdk.cairo_set_source_pixbuf(cr, self.pb, 0, 0)
        cr.paint()
        cr.set_source_rgba(0, 0, 0, 0.45)
        cr.paint()

        if self.start is None or self.cur is None:
            return False
        x0, y0 = self.start
        x1, y1 = self.cur
        x, y = min(x0, x1), min(y0, y1)
        w, h = abs(x1 - x0), abs(y1 - y0)
        if w < 1 or h < 1:
            return False
        cr.save()
        cr.rectangle(x, y, w, h)
        cr.clip()
        Gdk.cairo_set_source_pixbuf(cr, self.pb, 0, 0)
        cr.paint()
        cr.restore()
        cr.set_source_rgba(1, 1, 1, 0.9)
        cr.set_line_width(1)
        cr.rectangle(x + 0.5, y + 0.5, w, h)
        cr.stroke()
        label = "%d x %d" % (w, h)
        cr.set_font_size(12)
        cr.move_to(x + 3, y - 5 if y > 16 else y + h + 14)
        cr.set_source_rgba(0, 0, 0, 0.6)
        cr.set_line_width(3)
        cr.text_path(label)
        cr.stroke()
        cr.set_source_rgba(1, 1, 1, 1)
        cr.move_to(x + 3, y - 5 if y > 16 else y + h + 14)
        cr.show_text(label)
        return False


def select_region(pb):
    sel = Selector(pb)
    Gtk.main()
    rect = sel.rect
    sel.destroy()
    while Gtk.events_pending():
        Gtk.main_iteration()
    return rect


# --------------------------------------------------------------- редактор

class Editor(Gtk.Window):
    def __init__(self, surface, out_path):
        super().__init__(title="shot")
        self.surface = surface
        self.out_path = out_path
        self.iw = surface.get_width()
        self.ih = surface.get_height()
        self.basis = min(self.iw, self.ih)

        self.ops = []
        self.tool = "pen"
        self.pen_pct = PEN_DEFAULT
        self.txt_pct = TXT_DEFAULT
        self.current = None
        self.origin = (0.0, 0.0)
        self.pop = None
        self.saved = None

        self.connect("destroy", Gtk.main_quit)
        self.connect("key-press-event", self.on_key)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.pack_start(self.build_toolbar(), False, False, 0)
        self.area = Gtk.DrawingArea()
        self.area.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK
            | Gdk.EventMask.BUTTON_RELEASE_MASK
            | Gdk.EventMask.POINTER_MOTION_MASK
            | Gdk.EventMask.SCROLL_MASK
        )
        self.area.connect("draw", self.on_draw)
        self.area.connect("button-press-event", self.on_press)
        self.area.connect("motion-notify-event", self.on_motion)
        self.area.connect("button-release-event", self.on_release)
        self.area.connect("scroll-event", self.on_scroll)
        self.area.set_can_focus(True)
        box.pack_start(self.area, True, True, 0)
        self.add(box)

        disp = self.get_display()
        mon = disp.get_primary_monitor() or disp.get_monitor(0)
        geo = mon.get_geometry()
        sw = min(self.iw, int(geo.width * 0.95))
        sh = min(self.ih, int(geo.height * 0.85))
        self.set_default_size(sw + 2, sh + 44)
        self.show_all()
        self.area.grab_focus()
        self.update_cursor()

    # --- панель
    def build_toolbar(self):
        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        bar.set_margin_top(4)
        bar.set_margin_bottom(4)
        bar.set_margin_start(6)
        bar.set_margin_end(6)

        b_copy = Gtk.Button(label="копировать")
        b_copy.connect("clicked", self.on_copy)
        bar.pack_start(b_copy, False, False, 0)
        bar.pack_start(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL), False, False, 4)

        self.b_pen = Gtk.ToggleButton(label="перо")
        self.b_pen.connect("toggled", self.on_tool, "pen")
        bar.pack_start(self.b_pen, False, False, 0)
        self.b_text = Gtk.ToggleButton(label="текст")
        self.b_text.connect("toggled", self.on_tool, "text")
        bar.pack_start(self.b_text, False, False, 0)

        bar.pack_start(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL), False, False, 4)
        self.b_size = Gtk.Button(label="")
        self.b_size.connect("clicked", self.on_size_open)
        bar.pack_start(self.b_size, False, False, 0)

        self.b_done = Gtk.Button(label="готово")
        self.b_done.connect("clicked", self.on_done)
        bar.pack_end(self.b_done, False, False, 0)

        self.b_pen.set_active(True)
        self.update_size_label()
        return bar

    def update_size_label(self):
        if self.tool == "pen":
            self.b_size.set_label("толщина %.1f%%" % self.pen_pct)
        else:
            self.b_size.set_label("кегль %.1f%%" % self.txt_pct)

    def update_cursor(self):
        if getattr(self, "area", None) is None or self.area.get_window() is None:
            return
        name = "crosshair" if self.tool == "pen" else "text"
        cur = Gdk.Cursor.new_from_name(self.get_display(), name)
        self.area.get_window().set_cursor(cur)

    def on_tool(self, btn, tool):
        if not btn.get_active():
            if not self.b_pen.get_active() and not self.b_text.get_active():
                btn.set_active(True)
            return
        self.tool = tool
        other = self.b_text if tool == "pen" else self.b_pen
        if other.get_active():
            other.handler_block_by_func(self.on_tool)
            other.set_active(False)
            other.handler_unblock_by_func(self.on_tool)
        self.update_size_label()
        if getattr(self, "area", None) is not None and self.area.get_window():
            self.update_cursor()

    def on_size_open(self, btn):
        if self.pop is not None:
            self.pop.popdown()
            self.pop = None
            return
        pop = Gtk.Popover.new(btn)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.set_margin_top(8)
        box.set_margin_bottom(8)
        box.set_margin_start(10)
        box.set_margin_end(10)

        if self.tool == "pen":
            lo, hi, step, val, title = PEN_MIN, PEN_MAX, PEN_STEP, self.pen_pct, "толщина пера"
        else:
            lo, hi, step, val, title = TXT_MIN, TXT_MAX, TXT_STEP, self.txt_pct, "кегль текста"

        lab = Gtk.Label(label="%s, %% от min(w,h)" % title)
        box.pack_start(lab, False, False, 0)
        scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
        scale.set_digits(1)
        scale.set_value(val)
        scale.set_size_request(200, -1)
        scale.connect("value-changed", self.on_size_changed)
        box.pack_start(scale, False, False, 0)

        reset = Gtk.Button(label="по умолчанию")
        reset.connect("clicked", lambda _b: scale.set_value(
            PEN_DEFAULT if self.tool == "pen" else TXT_DEFAULT))
        box.pack_start(reset, False, False, 0)

        pop.add(box)
        pop.connect("closed", self.on_size_closed)
        pop.show_all()
        pop.popup()
        self.pop = pop

    def on_size_closed(self, _p):
        self.pop = None

    def on_size_changed(self, scale):
        val = round(scale.get_value(), 2)
        if self.tool == "pen":
            self.pen_pct = val
        else:
            self.txt_pct = val
        self.update_size_label()

    # --- геометрия
    def scale(self):
        aw = self.area.get_allocated_width()
        ah = self.area.get_allocated_height()
        if aw <= 1 or ah <= 1:
            return 1.0, 0.0, 0.0
        s = min(1.0, aw / self.iw, ah / self.ih)
        ox = (aw - self.iw * s) / 2.0
        oy = (ah - self.ih * s) / 2.0
        return s, ox, oy

    def to_image(self, ex, ey):
        s, ox, oy = self.scale()
        return (ex - ox) / s, (ey - oy) / s

    def on_draw(self, area, cr):
        s, ox, oy = self.scale()
        self.origin = (ox, oy)
        cr.set_source_rgba(1, 1, 1, 1)
        cr.paint()
        cr.save()
        cr.translate(ox, oy)
        cr.scale(s, s)
        cr.set_source_surface(self.surface, 0, 0)
        cr.paint()
        draw_ops(cr, self.ops, self.basis)
        if self.current is not None and self.current["kind"] == "stroke":
            draw_stroke(cr, self.current["pts"], self.current["w"])
        cr.restore()
        cr.set_source_rgba(0.5, 0.5, 0.5, 1)
        cr.set_line_width(1)
        cr.rectangle(ox - 0.5, oy - 0.5, self.iw * s + 1, self.ih * s + 1)
        cr.stroke()
        return False

    # --- мышь
    def on_press(self, _w, ev):
        if ev.button != 1:
            return False
        self.area.grab_focus()
        if os.environ.get("SHOT_DEBUG"):
            s_, ox_, oy_ = self.scale()
            sys.stderr.write("press win=(%.1f,%.1f) area=%dx%d s=%.4f ox=%.1f oy=%.1f\n" % (
                ev.x, ev.y, self.area.get_allocated_width(),
                self.area.get_allocated_height(), s_, ox_, oy_))
        x, y = self.to_image(ev.x, ev.y)
        if x < 0 or y < 0 or x > self.iw or y > self.ih:
            return False
        if self.tool == "pen":
            self.current = {
                "kind": "stroke",
                "pts": [(x, y)],
                "w": pen_px(self.pen_pct, self.basis),
            }
            self.queue_damage(x, y, self.current["w"])
            return True
        self.start_text_input(int(ev.x), int(ev.y), x, y)
        return True

    def on_motion(self, _w, ev):
        if self.current is None:
            return False
        x, y = self.to_image(ev.x, ev.y)
        x = max(0.0, min(x, self.iw))
        y = max(0.0, min(y, self.ih))
        self.current["pts"].append((x, y))
        self.queue_damage(x, y, self.current["w"])
        return True

    def on_release(self, _w, ev):
        if self.current is None:
            return False
        op = self.current
        self.current = None
        self.ops.append(op)
        self.area.queue_draw()
        return True

    def queue_damage(self, x, y, width):
        s, ox, oy = self.scale()
        r = width * s + 4
        self.area.queue_draw_area(
            int(ox + x * s - r), int(oy + y * s - r), int(2 * r), int(2 * r))

    def on_scroll(self, _w, ev):
        step = PEN_STEP if self.tool == "pen" else TXT_STEP
        up = ev.direction == Gdk.ScrollDirection.UP
        down = ev.direction == Gdk.ScrollDirection.DOWN
        if not (up or down):
            return False
        if self.tool == "pen":
            lo, hi = PEN_MIN, PEN_MAX
            self.pen_pct = round(min(hi, max(lo, self.pen_pct + (step if up else -step))), 2)
        else:
            lo, hi = TXT_MIN, TXT_MAX
            self.txt_pct = round(min(hi, max(lo, self.txt_pct + (step if up else -step))), 2)
        self.update_size_label()
        return True

    # --- текст
    def start_text_input(self, wx, wy, ix, iy):
        if self.pop is not None:
            self.pop.popdown()
            self.pop = None
        pop = Gtk.Popover.new(self.area)
        pop.set_pointing_to(Gdk.Rectangle(x=wx, y=wy, width=1, height=1))
        pop.set_position(Gtk.PositionType.RIGHT)
        entry = Gtk.Entry()
        entry.set_placeholder_text("текст, Enter — поставить")
        entry.set_size_request(260, -1)

        def commit(_e):
            val = entry.get_text()
            pop.popdown()
            if not val:
                return
            self.ops.append({
                "kind": "text",
                "x": ix,
                "y": iy,
                "text": val,
                "size": txt_px(self.txt_pct, self.basis),
            })
            self.area.queue_draw()

        def key(_e, ev):
            if ev.keyval == Gdk.KEY_Escape:
                pop.popdown()
                return True
            return False

        entry.connect("activate", commit)
        entry.connect("key-press-event", key)
        pop.add(entry)
        pop.connect("closed", self.on_size_closed)
        pop.show_all()
        pop.popup()
        entry.grab_focus()
        self.pop = pop

    # --- клавиши
    def on_key(self, _w, ev):
        ctrl = bool(ev.state & Gdk.ModifierType.CONTROL_MASK)
        if ctrl and ev.keyval in (Gdk.KEY_z, Gdk.KEY_Z):
            if self.ops:
                self.ops.pop()
                self.area.queue_draw()
            return True
        if ctrl and ev.keyval in (Gdk.KEY_c, Gdk.KEY_C):
            self.to_clipboard()
            return True
        if ev.keyval == Gdk.KEY_Escape:
            self.destroy()
            return True
        if ev.keyval == Gdk.KEY_1:
            self.b_pen.set_active(True)
            return True
        if ev.keyval == Gdk.KEY_2:
            self.b_text.set_active(True)
            return True
        return False

    # --- результат
    def render(self):
        surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, self.iw, self.ih)
        cr = cairo.Context(surf)
        cr.set_source_surface(self.surface, 0, 0)
        cr.paint()
        draw_ops(cr, self.ops, self.basis)
        surf.flush()
        return surf

    def png_bytes(self):
        buf = io.BytesIO()
        self.render().write_to_png(buf)
        return buf.getvalue()

    def to_clipboard(self):
        """В буфер через xclip: без менеджера обмена GTK-выбор умирает вместе с процессом."""
        data = self.png_bytes()
        if shutil.which("xclip"):
            try:
                proc = subprocess.Popen(
                    ["xclip", "-selection", "clipboard", "-t", "image/png", "-i"],
                    stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
                proc.communicate(data)
                return True
            except Exception as exc:  # noqa: BLE001
                sys.stderr.write("shot: xclip не сработал: %s\n" % exc)
        surf = self.render()
        pb = Gdk.pixbuf_get_from_surface(surf, 0, 0, self.iw, self.ih)
        clip = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        clip.set_image(pb)
        clip.store()
        return True

    def on_copy(self, _b):
        self.to_clipboard()

    def on_done(self, _b):
        if self.out_path:
            surf = self.render()
            try:
                surf.write_to_png(self.out_path)
            except Exception as exc:  # noqa: BLE001
                sys.stderr.write("shot: не смог записать %s: %s\n" % (self.out_path, exc))
                Gtk.main_quit()
                return
            self.saved = self.out_path
        else:
            self.to_clipboard()
        self.destroy()


# ------------------------------------------------------------------ запуск

def main():
    ap = argparse.ArgumentParser(
        prog="shot", description="скриншот с мини-редактором (перо + текст)")
    ap.add_argument("-s", "--select", action="store_true",
                    help="выбрать область мышью (как scrot -s)")
    ap.add_argument("-o", "--output", metavar="FILE",
                    help="сохранить в FILE вместо копирования в буфер")
    ap.add_argument("-d", "--delay", type=float, default=0.0,
                    help="задержка перед снимком, секунд")
    args = ap.parse_args()

    if Gdk.Display.get_default() is None:
        sys.stderr.write("shot: нет дисплея\n")
        return 2
    Gtk.init([])

    if args.delay > 0:
        time.sleep(args.delay)

    try:
        shot_pb = grab_screen()
    except RuntimeError as exc:
        sys.stderr.write("shot: %s\n" % exc)
        return 2

    if args.select:
        rect = select_region(shot_pb)
        if rect is None:
            return 1
        shot_pb = crop_pixbuf(shot_pb, *rect)

    surface = surface_from_pixbuf(shot_pb)
    ed = Editor(surface, args.output)
    Gtk.main()

    if args.output:
        if ed.saved == args.output:
            return 0
        if os.path.exists(args.output):
            return 0
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
