/*
 * xsnip — скриншот с мини-редактором.  C99, Xlib + Xft + zlib.
 *
 *   xsnip -s -o /tmp/a.png   выбрать область, отредактировать, сохранить
 *   xsnip -s                 выбрать область, отредактировать, в буфер
 *   xsnip -o /tmp/a.png      весь экран -> редактор -> файл
 *   xsnip                    весь экран -> редактор -> буфер
 *   xsnip -d 3               задержка перед снимком
 *
 *   --colors c1,c2,c3,c4,c5   палитра (#rrggbb), по умолчанию красный,
 *                             зелёный, жёлтый, белый, чёрный
 *   --color N|#hex            активный цвет (N от 1)
 *   --pen PCT                 толщина пера, % от min(w,h)  [0.1 .. 5]
 *   --text PCT                кегль текста, % от min(w,h)  [1 .. 50]
 *   --window dialog|normal    тип окна редактора
 *   --pos center|tl|tr|bl|br  положение окна редактора
 *
 * Настройки (цвет, размеры, инструмент, тип и место окна) хранятся в
 * $XDG_CONFIG_HOME/xsnip/config или ~/.config/xsnip/config.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xinerama.h>
#include <X11/keysym.h>

#include <zlib.h>

/* ------------------------------------------------------------------ общее */

#define NCOLORS 5
#define PANEL_H 46
#define MARGIN 10
#define TEXT_MAX 1024

static const char *DEF_COLORS[NCOLORS] = {
    "#e03131", "#2f9e44", "#f0b429", "#ffffff", "#000000",
};

#define PEN_MIN 0.1
#define PEN_MAX 5.0
#define PEN_STEP 0.1
#define PEN_DEF 1.0
#define TXT_MIN 1.0
#define TXT_MAX 50.0
#define TXT_STEP 0.5
#define TXT_DEF 10.0

static int x_error_handler(Display *d, XErrorEvent *ev) {
    (void)d;
    if (getenv("XSNIP_DEBUG"))
        fprintf(stderr, "X error: opcode=%d code=%d\n", ev->request_code, ev->error_code);
    return 0; /* не падаем: часть запросов гонки с WM */
}

static Display *dpy;
static int scr;
static Visual *visual;
static Colormap cmap;
static Window root;
static int depth;
static GC gc;
static XIM xim;
static XIC xic_editor;


/* ---------------------------------------------------------------- мониторы */

typedef struct {
    int x, y, w, h;
} mon_t;

static mon_t mons[16];
static int nmons;

static void monitors_init(void) {
    nmons = 0;
    if (XineramaIsActive(dpy)) {
        int n = 0;
        XineramaScreenInfo *si = XineramaQueryScreens(dpy, &n);
        if (si) {
            for (int i = 0; i < n && nmons < (int)(sizeof(mons) / sizeof(mons[0])); i++) {
                mons[nmons].x = si[i].x_org;
                mons[nmons].y = si[i].y_org;
                mons[nmons].w = si[i].width;
                mons[nmons].h = si[i].height;
                nmons++;
            }
            XFree(si);
        }
    }
    if (nmons == 0) {
        XWindowAttributes wa;
        XGetWindowAttributes(dpy, root, &wa);
        mons[0].x = 0;
        mons[0].y = 0;
        mons[0].w = wa.width;
        mons[0].h = wa.height;
        nmons = 1;
    }
    if (getenv("XSNIP_DEBUG")) {
        for (int i = 0; i < nmons; i++)
            fprintf(stderr, "monitor %d: %dx%d+%d+%d\n", i, mons[i].w, mons[i].h,
                    mons[i].x, mons[i].y);
    }
}

/* экран, на котором лежит точка (курсор или центр выделения) */
static mon_t monitor_at(int x, int y) {
    for (int i = 0; i < nmons; i++)
        if (x >= mons[i].x && x < mons[i].x + mons[i].w && y >= mons[i].y &&
            y < mons[i].y + mons[i].h)
            return mons[i];
    return mons[0];
}

static void pointer_pos(int *x, int *y) {
    Window r, c;
    int wx, wy;
    unsigned mask;
    *x = *y = 0;
    XQueryPointer(dpy, root, &r, &c, x, y, &wx, &wy, &mask);
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("xsnip: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

/* --------------------------------------------------------------- картинка */

typedef struct {
    int w, h;
    uint32_t *px; /* 0xRRGGBB */
} img_t;

static img_t *img_new(int w, int h) {
    img_t *im = calloc(1, sizeof(*im));
    if (!im)
        die("нет памяти");
    im->w = w;
    im->h = h;
    im->px = calloc((size_t)w * h, sizeof(uint32_t));
    if (!im->px)
        die("нет памяти (%dx%d)", w, h);
    return im;
}

static void img_free(img_t *im) {
    if (!im)
        return;
    free(im->px);
    free(im);
}

/*
 * Маски в XImage, полученном из Pixmap, приходят нулевыми — берём их у визуала.
 */
static uint32_t pixel_from_ximage(XImage *xi, int x, int y) {
    unsigned long rm = xi->red_mask ? xi->red_mask : visual->red_mask;
    unsigned long gm = xi->green_mask ? xi->green_mask : visual->green_mask;
    unsigned long bm = xi->blue_mask ? xi->blue_mask : visual->blue_mask;
    if (!rm) rm = 0xff0000;
    if (!gm) gm = 0xff00;
    if (!bm) bm = 0xff;
    unsigned long p = XGetPixel(xi, x, y);
    unsigned r = (unsigned)((p & rm) >> __builtin_ctzl(rm)) & 0xff;
    unsigned g = (unsigned)((p & gm) >> __builtin_ctzl(gm)) & 0xff;
    unsigned b = (unsigned)((p & bm) >> __builtin_ctzl(bm)) & 0xff;
    return (r << 16) | (g << 8) | b;
}

static img_t *capture_root(void) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, root, &wa))
        die("нет доступа к корневому окну");
    XImage *xi = XGetImage(dpy, root, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
    if (!xi)
        die("не удалось снять экран");

    img_t *im = img_new(wa.width, wa.height);
    int fast = (xi->bits_per_pixel == 32 && xi->byte_order == LSBFirst &&
                visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 &&
                visual->blue_mask == 0xff);
    if (fast) {
        for (int y = 0; y < im->h; y++) {
            uint32_t *src = (uint32_t *)(xi->data + (size_t)y * xi->bytes_per_line);
            uint32_t *dst = im->px + (size_t)y * im->w;
            for (int x = 0; x < im->w; x++)
                dst[x] = src[x] & 0xffffffu;
        }
    } else {
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++)
                im->px[(size_t)y * im->w + x] = pixel_from_ximage(xi, x, y);
    }
    XDestroyImage(xi);
    return im;
}

static img_t *img_crop(const img_t *src, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > src->w) w = src->w - x;
    if (y + h > src->h) h = src->h - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    img_t *im = img_new(w, h);
    for (int r = 0; r < h; r++)
        memcpy(im->px + (size_t)r * w, src->px + (size_t)(y + r) * src->w + x,
               (size_t)w * sizeof(uint32_t));
    return im;
}

/* XImage, владеющий копией пикселей картинки */
static XImage *img_to_ximage(const img_t *im) {
    size_t bytes = (size_t)im->w * im->h * 4;
    char *data = malloc(bytes);
    if (!data)
        die("нет памяти под XImage");
    memcpy(data, im->px, bytes);
    XImage *xi = XCreateImage(dpy, visual, depth, ZPixmap, 0, data, im->w, im->h, 32,
                              im->w * 4);
    if (!xi)
        die("не удалось создать XImage");
    return xi;
}

/* ------------------------------------------------------------ цвет и конфиг */

static int parse_color(const char *s, uint32_t *out) {
    if (!s || s[0] != '#')
        return 0;
    size_t n = strlen(s + 1);
    unsigned r, g, b;
    if (n == 6) {
        if (sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) != 3)
            return 0;
    } else if (n == 3) {
        unsigned rr, gg, bb;
        if (sscanf(s + 1, "%1x%1x%1x", &rr, &gg, &bb) != 3)
            return 0;
        r = rr * 17; g = gg * 17; b = bb * 17;
    } else {
        return 0;
    }
    *out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return 1;
}

static void color_to_str(uint32_t c, char *buf, size_t n) {
    snprintf(buf, n, "#%06x", c & 0xffffffu);
}

typedef struct {
    uint32_t color;   /* активный цвет */
    char colors[NCOLORS][8];
    double pen, txt;
    int tool;         /* 0 перо, 1 текст */
    int wintype;      /* 0 dialog, 1 normal */
    int pos;          /* 0 center, 1 tl, 2 tr, 3 bl, 4 br */
} cfg_t;

static const char *POS_NAMES[5] = {"center", "tl", "tr", "bl", "br"};

static void config_path(char *buf, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/xsnip/config", xdg);
    else
        snprintf(buf, n, "%s/.config/xsnip/config", home ? home : ".");
}

static void cfg_defaults(cfg_t *c) {
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < NCOLORS; i++)
        snprintf(c->colors[i], sizeof(c->colors[i]), "%s", DEF_COLORS[i]);
    parse_color(c->colors[0], &c->color);
    c->pen = PEN_DEF;
    c->txt = TXT_DEF;
    c->tool = 0;
    c->wintype = 0;
    c->pos = 0;
}

static void cfg_load(cfg_t *c) {
    char path[512];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        char *nl = strchr(val, '\n');
        if (nl)
            *nl = 0;
        if (strcmp(key, "color") == 0)
            parse_color(val, &c->color);
        else if (strcmp(key, "pen") == 0)
            c->pen = atof(val);
        else if (strcmp(key, "text") == 0)
            c->txt = atof(val);
        else if (strcmp(key, "tool") == 0)
            c->tool = strcmp(val, "text") == 0 ? 1 : (strcmp(val, "crop") == 0 ? 2 : 0);
        else if (strcmp(key, "window") == 0)
            c->wintype = strcmp(val, "normal") == 0 ? 1 : 0;
        else if (strcmp(key, "pos") == 0) {
            for (int i = 0; i < 5; i++)
                if (strcmp(val, POS_NAMES[i]) == 0)
                    c->pos = i;
        }
    }
    fclose(f);
    if (c->pen < PEN_MIN) c->pen = PEN_MIN;
    if (c->pen > PEN_MAX) c->pen = PEN_MAX;
    if (c->txt < TXT_MIN) c->txt = TXT_MIN;
    if (c->txt > TXT_MAX) c->txt = TXT_MAX;
}

/* printf уважает локаль (в ru_RU дробь — запятая), а в конфиге нужна точка */
static void fmt_num(char *buf, size_t n, double v) {
    snprintf(buf, n, "%.2f", v);
    for (char *p = buf; *p; p++)
        if (*p == ',')
            *p = '.';
}

static void cfg_save(const cfg_t *c) {
    char path[512], tmp[560];
    config_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0) { /* каталог не создался — молча продолжаем */ }
    }
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    char hex[8];
    color_to_str(c->color, hex, sizeof(hex));
    fprintf(f, "color=%s\n", hex);
    char num[32];
    fmt_num(num, sizeof(num), c->pen);
    fprintf(f, "pen=%s\n", num);
    fmt_num(num, sizeof(num), c->txt);
    fprintf(f, "text=%s\n", num);
    fprintf(f, "tool=%s\n", c->tool == 1 ? "text" : (c->tool == 2 ? "crop" : "pen"));
    fprintf(f, "window=%s\n", c->wintype ? "normal" : "dialog");
    fprintf(f, "pos=%s\n", POS_NAMES[c->pos]);
    fclose(f);
    if (rename(tmp, path) != 0)
        unlink(tmp);
}

/* ---------------------------------------------------------------------- PNG */

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

typedef struct {
    unsigned char *data;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n)
            cap *= 2;
        unsigned char *nd = realloc(b->data, cap);
        if (!nd)
            die("нет памяти (png)");
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void png_chunk(buf_t *b, const char *type, const unsigned char *data, size_t n) {
    unsigned char hdr[8];
    put32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    buf_add(b, hdr, 8);
    if (n)
        buf_add(b, data, n);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n)
        crc = crc32(crc, data, (uInt)n);
    unsigned char tail[4];
    put32(tail, (uint32_t)crc);
    buf_add(b, tail, 4);
}

static int png_encode(const img_t *im, unsigned char **out, size_t *outlen) {
    size_t rawlen = (size_t)im->h * (1 + (size_t)im->w * 3);
    unsigned char *raw = malloc(rawlen);
    if (!raw)
        return -1;
    size_t o = 0;
    for (int y = 0; y < im->h; y++) {
        raw[o++] = 0; /* filter: none */
        const uint32_t *row = im->px + (size_t)y * im->w;
        for (int x = 0; x < im->w; x++) {
            uint32_t p = row[x];
            raw[o++] = (p >> 16) & 0xff;
            raw[o++] = (p >> 8) & 0xff;
            raw[o++] = p & 0xff;
        }
    }
    uLongf zlen = compressBound(rawlen);
    unsigned char *zdata = malloc(zlen);
    if (!zdata) {
        free(raw);
        return -1;
    }
    if (compress2(zdata, &zlen, raw, rawlen, 6) != Z_OK) {
        free(raw);
        free(zdata);
        return -1;
    }
    free(raw);

    buf_t b = {0};
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    buf_add(&b, sig, 8);

    unsigned char ihdr[13];
    put32(ihdr, (uint32_t)im->w);
    put32(ihdr + 4, (uint32_t)im->h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    png_chunk(&b, "IHDR", ihdr, 13);
    png_chunk(&b, "IDAT", zdata, zlen);
    png_chunk(&b, "IEND", NULL, 0);
    free(zdata);

    *out = b.data;
    *outlen = b.len;
    return 0;
}

static int png_write_file(const img_t *im, const char *path) {
    unsigned char *data;
    size_t len;
    if (png_encode(im, &data, &len) != 0) {
        fprintf(stderr, "xsnip: не смог собрать PNG\n");
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "xsnip: %s: %s\n", path, strerror(errno));
        free(data);
        return -1;
    }
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    return wr == len ? 0 : -1;
}

/* --------------------------------------------------- буфер: X11 selection */

/*
 * Держим выделение в форкнутом потомке: без менеджера обмена содержимое
 * буфера умирает вместе с процессом. Данные отдаём как image/png, а если
 * они не влезают в одно свойство — по протоколу INCR.
 */

static Atom A_CLIPBOARD, A_TARGETS, A_INCR, A_PNG_IMAGE, A_PNG_UPPER, A_TIMESTAMP;

typedef struct {
    Window req;
    Atom prop;
    size_t off;
    int active;
} incr_t;

static void notify(Window req, Atom prop, Atom target, Time t) {
    XSelectionEvent ev = {0};
    ev.type = SelectionNotify;
    ev.display = dpy;
    ev.requestor = req;
    ev.selection = A_CLIPBOARD;
    ev.target = target;
    ev.property = prop;
    ev.time = t;
    XSendEvent(dpy, req, False, 0, (XEvent *)&ev);
    XFlush(dpy);
}

static size_t max_prop_bytes(void) {
    long ext = XExtendedMaxRequestSize(dpy);
    if (ext <= 0)
        ext = XMaxRequestSize(dpy);
    size_t m = (size_t)ext * 4;
    if (m > 262144)
        m = 262144;
    if (m < 8192)
        m = 8192;
    return m;
}

static void clipboard_daemon(unsigned char *data, size_t len) {
    /* вызывается уже в потомке */
    Window win = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    if (!win)
        _exit(1);
    XSelectInput(dpy, win, PropertyChangeMask);
    XSetSelectionOwner(dpy, A_CLIPBOARD, win, CurrentTime);
    if (XGetSelectionOwner(dpy, A_CLIPBOARD) != win)
        _exit(1);
    XFlush(dpy);

    incr_t inc = {0};
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case SelectionRequest: {
            XSelectionRequestEvent *rq = &ev.xselectionrequest;
            Atom prop = rq->property ? rq->property : rq->target;
            if (rq->target == A_TARGETS) {
                Atom list[4] = {A_TARGETS, A_PNG_IMAGE, A_PNG_UPPER, XA_STRING};
                XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                                (unsigned char *)list, 4);
                notify(rq->requestor, prop, rq->target, rq->time);
            } else if (rq->target == A_PNG_IMAGE || rq->target == A_PNG_UPPER) {
                if (len <= max_prop_bytes()) {
                    XChangeProperty(dpy, rq->requestor, prop, A_PNG_IMAGE, 8,
                                    PropModeReplace, data, (int)len);
                    notify(rq->requestor, prop, rq->target, rq->time);
                } else {
                    /* INCR: сначала объявляем длину, потом льём кусками */
                    long n = (long)len;
                    XSelectInput(dpy, rq->requestor, PropertyChangeMask);
                    XChangeProperty(dpy, rq->requestor, prop, A_INCR, 32,
                                    PropModeReplace, (unsigned char *)&n, 1);
                    notify(rq->requestor, prop, rq->target, rq->time);
                    inc.req = rq->requestor;
                    inc.prop = prop;
                    inc.off = 0;
                    inc.active = 1;
                }
            } else {
                notify(rq->requestor, None, rq->target, rq->time);
            }
            break;
        }
        case PropertyNotify: {
            if (!inc.active || ev.xproperty.window != inc.req ||
                ev.xproperty.atom != inc.prop ||
                ev.xproperty.state != PropertyDelete)
                break;
            size_t chunk = max_prop_bytes();
            size_t left = len - inc.off;
            size_t n = left < chunk ? left : chunk;
            XChangeProperty(dpy, inc.req, inc.prop, A_PNG_IMAGE, 8, PropModeReplace,
                            data + inc.off, (int)n);
            inc.off += n;
            if (n == 0)
                inc.active = 0;
            break;
        }
        case SelectionClear:
            _exit(0);
        default:
            break;
        }
    }
}

/* Копирует PNG в буфер: потомок живёт, пока данные не заберут. */
static void clipboard_put_png(const unsigned char *data, size_t len) {
    unsigned char *copy = malloc(len);
    if (!copy)
        die("нет памяти под буфер");
    memcpy(copy, data, len);

    pid_t pid = fork();
    if (pid < 0) {
        free(copy);
        fprintf(stderr, "xsnip: fork: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2)
                close(devnull);
        }
        /* Унаследованное от родителя X-соединение трогать нельзя: fd общий,
         * и любой запрос из потомка разрушит протокол родителя. Открываем своё,
         * старое просто перестаём использовать (умрёт вместе с демоном). */
        Display *nd = XOpenDisplay(NULL);
        if (!nd)
            _exit(1);
        dpy = nd;
        clipboard_daemon(copy, len);
        _exit(0);
    }
    /* родитель: не ждём, потомок сам держит выделение */
    free(copy);
    int status;
    waitpid(pid, &status, WNOHANG);
}

/* ---------------------------------------------------------- выбор области */

typedef struct {
    int x, y, w, h;
    int ok;
} rect_t;

typedef struct {
    Window win;
    img_t *shot;
    XImage *bright, *dark;
    int x0, y0, x1, y1;
    int dragging;
    GC gc;
    XftDraw *draw;
    XftFont *font;
    int done;
    rect_t rect;
} sel_t;

static void sel_damage(sel_t *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->shot->w) w = s->shot->w - x;
    if (y + h > s->shot->h) h = s->shot->h - y;
    if (w <= 0 || h <= 0)
        return;
    XPutImage(dpy, s->win, s->gc, s->dark, x, y, x, y, w, h);
}

static int sel_rect_now(sel_t *s, int *rx, int *ry, int *rw, int *rh) {
    if (!s->dragging)
        return 0;
    int x = s->x0 < s->x1 ? s->x0 : s->x1;
    int y = s->y0 < s->y1 ? s->y0 : s->y1;
    int w = abs(s->x1 - s->x0), h = abs(s->y1 - s->y0);
    if (w < 1 || h < 1)
        return 0;
    *rx = x; *ry = y; *rw = w; *rh = h;
    return 1;
}

static void sel_draw(sel_t *s) {
    int x, y, w, h;
    if (!sel_rect_now(s, &x, &y, &w, &h))
        return;
    XPutImage(dpy, s->win, s->gc, s->bright, x, y, x, y, w, h);
    XSetForeground(dpy, s->gc, 0xffffff);
    XSetLineAttributes(dpy, s->gc, 1, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, s->win, s->gc, x, y, w - 1, h - 1);

    char label[64];
    snprintf(label, sizeof(label), "%d x %d", w, h);
    int ty = (y > 20) ? y - 6 : y + h + 16;
    XSetForeground(dpy, s->gc, 0x000000);
    int bl = ty + s->font->ascent / 2;
    XftColor black = {0}, white = {0};
    XRenderColor rc;
    rc.red = 0; rc.green = 0; rc.blue = 0; rc.alpha = 0xffff;
    XftColorAllocValue(dpy, visual, cmap, &rc, &black);
    rc.red = rc.green = rc.blue = 0xffff;
    XftColorAllocValue(dpy, visual, cmap, &rc, &white);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy)
                XftDrawStringUtf8(s->draw, &black, s->font, x + 4 + dx, bl + dy,
                                  (const FcChar8 *)label, (int)strlen(label));
    XftDrawStringUtf8(s->draw, &white, s->font, x + 4, bl, (const FcChar8 *)label,
                      (int)strlen(label));
    XftColorFree(dpy, visual, cmap, &black);
    XftColorFree(dpy, visual, cmap, &white);
    (void)bl;
}

static rect_t select_area(img_t *shot) {
    sel_t s = {0};
    s.shot = shot;
    s.win = XCreateSimpleWindow(dpy, root, 0, 0, shot->w, shot->h, 0, 0, 0);
    if (!s.win)
        die("не удалось создать окно выделения");

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = 0;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask;
    XChangeWindowAttributes(dpy, s.win,
                            CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

    s.bright = img_to_ximage(shot);
    img_t *dim = img_new(shot->w, shot->h);
    for (size_t i = 0; i < (size_t)shot->w * shot->h; i++) {
        uint32_t p = shot->px[i];
        uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
        r = r * 55 / 100;
        g = g * 55 / 100;
        b = b * 55 / 100;
        dim->px[i] = (r << 16) | (g << 8) | b;
    }
    s.dark = img_to_ximage(dim);
    img_free(dim);

    s.gc = XCreateGC(dpy, s.win, 0, NULL);
    s.draw = XftDrawCreate(dpy, s.win, visual, cmap);
    s.font = XftFontOpenName(dpy, scr, "sans:pixelsize=13");

    Cursor cur = XCreateFontCursor(dpy, XC_crosshair);
    XDefineCursor(dpy, s.win, cur);

    XStoreName(dpy, s.win, "xsnip-select");
    XMapRaised(dpy, s.win);
    XSync(dpy, False);
    XSetInputFocus(dpy, s.win, RevertToPointerRoot, CurrentTime);
    XGrabPointer(dpy, s.win, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, cur, CurrentTime);
    XGrabKeyboard(dpy, s.win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    XPutImage(dpy, s.win, s.gc, s.dark, 0, 0, 0, 0, shot->w, shot->h);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                XPutImage(dpy, s.win, s.gc, s.dark, 0, 0, 0, 0, shot->w, shot->h);
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button3) {
                s.done = 1;
                s.rect.ok = 0;
                goto out;
            }
            if (ev.xbutton.button != Button1)
                break;
            s.x0 = s.x1 = ev.xbutton.x;
            s.y0 = s.y1 = ev.xbutton.y;
            s.dragging = 1;
            break;
        case MotionNotify: {
            int x, y, w, h;
            int ox = 0, oy = 0, ow = 0, oh = 0;
            sel_rect_now(&s, &ox, &oy, &ow, &oh);
            s.x1 = ev.xmotion.x;
            s.y1 = ev.xmotion.y;
            int nx, ny, nw, nh;
            int has_new = sel_rect_now(&s, &nx, &ny, &nw, &nh);
            x = ox; y = oy; w = ow; h = oh;
            if (has_new) {
                int minx = x < nx ? x : nx, miny = y < ny ? y : ny;
                int maxx = (x + w) > (nx + nw) ? (x + w) : (nx + nw);
                int maxy = (y + h) > (ny + nh) ? (y + h) : (ny + nh);
                x = minx - 4; y = miny - 24; w = maxx - minx + 8; h = maxy - miny + 48;
            }
            sel_damage(&s, x, y, w, h);
            sel_draw(&s);
            break;
        }
        case ButtonRelease: {
            int x, y, w, h;
            if (!sel_rect_now(&s, &x, &y, &w, &h)) {
                s.done = 1;
                s.rect.ok = 0;
                goto out;
            }
            s.rect.x = x;
            s.rect.y = y;
            s.rect.w = w;
            s.rect.h = h;
            s.rect.ok = (w >= 5 && h >= 5);
            s.done = 1;
            goto out;
        }
        case KeyPress: {
            KeySym ks = 0;
            char buf[32];
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Escape) {
                s.done = 1;
                s.rect.ok = 0;
                goto out;
            }
            break;
        }
        default:
            break;
        }
    }
out:
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XftFontClose(dpy, s.font);
    XftDrawDestroy(s.draw);
    XFreeGC(dpy, s.gc);
    XDestroyWindow(dpy, s.win);
    XFreeCursor(dpy, cur);
    XDestroyImage(s.bright);
    XDestroyImage(s.dark);
    XSync(dpy, False);
    return s.rect;
}

/* ------------------------------------------------------------------ шрифты */

typedef struct {
    int px;
    XftFont *font;
} fontent_t;

static fontent_t fonts[32];
static int nfonts;

static XftFont *font_get(int px) {
    if (px < 4)
        px = 4;
    for (int i = 0; i < nfonts; i++)
        if (fonts[i].px == px)
            return fonts[i].font;
    char spec[64];
    snprintf(spec, sizeof(spec), "sans:pixelsize=%d", px);
    XftFont *f = XftFontOpenName(dpy, scr, spec);
    if (!f && px > 8)
        f = XftFontOpenName(dpy, scr, "sans:pixelsize=13");
    if (!f)
        die("не нашёл шрифт (fontconfig)");
    if (nfonts < (int)(sizeof(fonts) / sizeof(fonts[0]))) {
        fonts[nfonts].px = px;
        fonts[nfonts].font = f;
        nfonts++;
    }
    return f;
}

static void xft_color(uint32_t rgb, XftColor *out) {
    XRenderColor rc;
    rc.red = ((rgb >> 16) & 0xff) * 0x101;
    rc.green = ((rgb >> 8) & 0xff) * 0x101;
    rc.blue = (rgb & 0xff) * 0x101;
    rc.alpha = 0xffff;
    if (!XftColorAllocValue(dpy, visual, cmap, &rc, out))
        memset(out, 0, sizeof(*out));
}

static void draw_str(XftDraw *draw, uint32_t rgb, int px, int x, int y, const char *s) {
    XftFont *f = font_get(px);
    XftColor col;
    xft_color(rgb, &col);
    XftDrawStringUtf8(draw, &col, f, x, y + f->ascent, (const FcChar8 *)s, (int)strlen(s));
    XftColorFree(dpy, visual, cmap, &col);
}

static int str_width(int px, const char *s) {
    XGlyphInfo ext;
    XftFont *f = font_get(px);
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}

/* ------------------------------------------------------------------- ops */

enum { OP_STROKE = 0, OP_TEXT = 1 };

typedef struct {
    int kind;
    uint32_t color;
    double width;   /* пиксели изображения, для штриха */
    int size;       /* кегль в пикселях изображения, для текста */
    int n;          /* число точек */
    int *pts;       /* x0,y0,x1,y1,... в координатах изображения */
    char *text;
    int tx, ty;     /* левый верх текста */
} op_t;

typedef struct {
    op_t *items;
    int n, cap;
} ops_t;

static op_t *ops_push(ops_t *o) {
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        o->items = realloc(o->items, (size_t)o->cap * sizeof(op_t));
        if (!o->items)
            die("нет памяти (ops)");
    }
    op_t *op = &o->items[o->n++];
    memset(op, 0, sizeof(*op));
    return op;
}

static void ops_free(ops_t *o) {
    for (int i = 0; i < o->n; i++) {
        free(o->items[i].pts);
        free(o->items[i].text);
    }
    free(o->items);
    memset(o, 0, sizeof(*o));
}

/* ---------------------------------------------------------------- редактор */

enum {
    B_COPY = 1,
    B_UNDO,
    B_PEN,
    B_TEXT,
    B_CROP,
    B_SHRINK,
    B_GROW,
    B_DONE,
    B_COLOR0 = 100
};

typedef struct {
    int id, x, y, w, h;
} btn_t;

typedef struct {
    Window win;
    img_t *src;      /* исходное изображение */
    img_t *view;     /* масштабированный фон канваса (кэш) */
    XImage *view_xi;
    ops_t ops;
    cfg_t cfg;
    int cw, ch;      /* размер канваса */
    double scale;
    int ox, oy;      /* смещение картинки внутри канваса */
    int iw, ih;      /* размер изображения */
    int basis;
    const char *outpath;
    int saved;
    int done;

    btn_t btns[32];
    int nbtn;
    int hot;         /* кнопка под курсором */
    int pressed;     /* нажатая кнопка */

    int drawing;
    int *cur_pts;
    int cur_n, cur_cap;
    double cur_w;
    uint32_t cur_color;

    int crop_active;   /* тянем рамку вырезания */
    int cx0, cy0, cx1, cy1;   /* рамка в координатах изображения */

    int text_active;
    char textbuf[TEXT_MAX];
    int text_len;
    int text_x, text_y;   /* координаты вставки в изображении */
    int text_wi;          /* ширина ввода в окне */

    mon_t mon;       /* экран, на котором живёт окно */
    int cursor_zone; /* 0 — панель, 1 — картинка */

    XftDraw *draw;
    GC cgc;
    Cursor cur_pen, cur_text, cur_arrow;
    Window wm_delete;
} editor_t;

static void editor_relayout(editor_t *e);
static void editor_repaint(editor_t *e);
static void editor_crop_apply(editor_t *e);

static void view_rebuild(editor_t *e) {
    if (e->view_xi) {
        XDestroyImage(e->view_xi);
        e->view_xi = NULL;
    }
    img_free(e->view);
    e->view = NULL;
    if (e->cw < 2 || e->ch < 2)
        return;

    double s = 1.0;
    double sx = (double)e->cw / e->iw;
    double sy = (double)e->ch / e->ih;
    if (sx < s) s = sx;
    if (sy < s) s = sy;
    e->scale = s;
    e->ox = (int)((e->cw - e->iw * s) / 2.0);
    e->oy = (int)((e->ch - e->ih * s) / 2.0);

    img_t *v = img_new(e->cw, e->ch);
    for (size_t i = 0; i < (size_t)e->cw * e->ch; i++)
        v->px[i] = 0x9a9a96;
    /* bilinear */
    for (int y = 0; y < e->ch; y++) {
        double iy = (y - e->oy) / s;
        if (iy < -0.5 || iy > e->ih - 0.5)
            continue;
        if (iy < 0) iy = 0;
        int y0 = (int)iy, y1 = y0 + 1;
        if (y1 >= e->ih) y1 = e->ih - 1;
        double fy = iy - y0;
        for (int x = 0; x < e->cw; x++) {
            double ix = (x - e->ox) / s;
            if (ix < -0.5 || ix > e->iw - 0.5)
                continue;
            if (ix < 0) ix = 0;
            int x0 = (int)ix, x1 = x0 + 1;
            if (x1 >= e->iw) x1 = e->iw - 1;
            double fx = ix - x0;
            const uint32_t *r0 = e->src->px + (size_t)y0 * e->iw;
            const uint32_t *r1 = e->src->px + (size_t)y1 * e->iw;
            uint32_t p00 = r0[x0], p01 = r0[x1], p10 = r1[x0], p11 = r1[x1];
            double rr = 0, gg = 0, bb = 0;
            double w00 = (1 - fx) * (1 - fy), w01 = fx * (1 - fy);
            double w10 = (1 - fx) * fy, w11 = fx * fy;
            rr = w00 * ((p00 >> 16) & 0xff) + w01 * ((p01 >> 16) & 0xff) +
                 w10 * ((p10 >> 16) & 0xff) + w11 * ((p11 >> 16) & 0xff);
            gg = w00 * ((p00 >> 8) & 0xff) + w01 * ((p01 >> 8) & 0xff) +
                 w10 * ((p10 >> 8) & 0xff) + w11 * ((p11 >> 8) & 0xff);
            bb = w00 * (p00 & 0xff) + w01 * (p01 & 0xff) + w10 * (p10 & 0xff) +
                 w11 * (p11 & 0xff);
            v->px[(size_t)y * e->cw + x] =
                ((uint32_t)(rr + 0.5) << 16) | ((uint32_t)(gg + 0.5) << 8) |
                (uint32_t)(bb + 0.5);
        }
    }
    e->view = v;
    e->view_xi = img_to_ximage(v);
}

static void editor_draw_ops(editor_t *e, Drawable d, XftDraw *xd, int to_window) {
    double s = to_window ? e->scale : 1.0;
    int ox = to_window ? e->ox : 0;
    int oy = to_window ? e->oy + PANEL_H : 0;
    for (int i = 0; i < e->ops.n; i++) {
        op_t *op = &e->ops.items[i];
        if (op->kind == OP_STROKE) {
            int lw = (int)(op->width * s + 0.5);
            if (lw < 1)
                lw = 1;
            XSetForeground(dpy, e->cgc, op->color);
            XSetLineAttributes(dpy, e->cgc, lw, LineSolid, CapRound, JoinRound);
            if (op->n == 1) {
                int x = ox + (int)(op->pts[0] * s), y = oy + (int)(op->pts[1] * s);
                XFillArc(dpy, d, e->cgc, x - lw / 2, y - lw / 2, lw, lw, 0, 360 * 64);
            } else {
                for (int k = 1; k < op->n; k++) {
                    int x0 = ox + (int)(op->pts[(k - 1) * 2] * s);
                    int y0 = oy + (int)(op->pts[(k - 1) * 2 + 1] * s);
                    int x1 = ox + (int)(op->pts[k * 2] * s);
                    int y1 = oy + (int)(op->pts[k * 2 + 1] * s);
                    XDrawLine(dpy, d, e->cgc, x0, y0, x1, y1);
                }
            }
        } else if (op->kind == OP_TEXT && op->text) {
            int px = (int)(op->size * s + 0.5);
            if (px < 4)
                px = 4;
            draw_str(xd, op->color, px, ox + (int)(op->tx * s),
                     oy + (int)(op->ty * s), op->text);
        }
    }
    if (to_window && e->drawing && e->cur_n > 0) {
        int lw = (int)(e->cur_w * s + 0.5);
        if (lw < 1)
            lw = 1;
        XSetForeground(dpy, e->cgc, e->cur_color);
        XSetLineAttributes(dpy, e->cgc, lw, LineSolid, CapRound, JoinRound);
        for (int k = 1; k < e->cur_n; k++) {
            int x0 = ox + (int)(e->cur_pts[(k - 1) * 2] * s);
            int y0 = oy + (int)(e->cur_pts[(k - 1) * 2 + 1] * s);
            int x1 = ox + (int)(e->cur_pts[k * 2] * s);
            int y1 = oy + (int)(e->cur_pts[k * 2 + 1] * s);
            XDrawLine(dpy, d, e->cgc, x0, y0, x1, y1);
        }
    }
}

/* --- иконки: рисуем примитивами, никаких картинок и зависимостей */

enum { ICON_NONE = 0, ICON_UNDO, ICON_COPY, ICON_CHECK, ICON_PEN, ICON_TEXT, ICON_CROP };

static void icon_stroke(editor_t *e, uint32_t col, int w) {
    XSetForeground(dpy, e->cgc, col);
    XSetLineAttributes(dpy, e->cgc, w, LineSolid, CapRound, JoinRound);
}

static void draw_icon(editor_t *e, Drawable d, int icon, int cx, int cy, int sz,
                      uint32_t col) {
    int r = sz / 2;
    int lw = sz - 6;
    switch (icon) {
    case ICON_UNDO: {
        icon_stroke(e, col, 2);
        XDrawArc(dpy, d, e->cgc, cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2,
                 40 * 64, 260 * 64);
        int hx = cx - r + 2, hy = cy - r + 3;
        XPoint head[3] = {{hx - 2, hy}, {hx + 5, hy - 3}, {hx + 5, hy + 4}};
        XSetForeground(dpy, e->cgc, col);
        XFillPolygon(dpy, d, e->cgc, head, 3, Convex, CoordModeOrigin);
        break;
    }
    case ICON_COPY: {
        /* задний лист — только уголок справа сверху, передний — контуром */
        icon_stroke(e, col, 1);
        XDrawRectangle(dpy, d, e->cgc, cx - r + 5, cy - r + 1, lw - 2, lw - 2);
        XSetForeground(dpy, e->cgc, 0xffffff);
        XFillRectangle(dpy, d, e->cgc, cx - r + 1, cy - r + 4, lw - 1, lw - 1);
        XSetForeground(dpy, e->cgc, col);
        XDrawRectangle(dpy, d, e->cgc, cx - r + 1, cy - r + 4, lw - 1, lw - 1);
        break;
    }
    case ICON_CHECK: {
        icon_stroke(e, col, 3);
        XDrawLine(dpy, d, e->cgc, cx - 6, cy, cx - 1, cy + 5);
        XDrawLine(dpy, d, e->cgc, cx - 1, cy + 5, cx + 7, cy - 6);
        break;
    }
    case ICON_PEN: {
        icon_stroke(e, col, 2);
        XDrawLine(dpy, d, e->cgc, cx - 6, cy + 6, cx + 2, cy - 2);
        XPoint tip[3] = {{cx + 6, cy - 6}, {cx + 1, cy - 3}, {cx + 3, cy - 1}};
        XSetForeground(dpy, e->cgc, col);
        XFillPolygon(dpy, d, e->cgc, tip, 3, Convex, CoordModeOrigin);
        break;
    }
    case ICON_TEXT: {
        icon_stroke(e, col, 2);
        XDrawLine(dpy, d, e->cgc, cx - 6, cy - 6, cx + 6, cy - 6);
        XDrawLine(dpy, d, e->cgc, cx, cy - 6, cx, cy + 6);
        break;
    }
    case ICON_CROP: {
        icon_stroke(e, col, 2);
        /* две уголковые скобки наискосок — как значок «обрезать» */
        XDrawLine(dpy, d, e->cgc, cx - 6, cy - 2, cx - 6, cy - 6);
        XDrawLine(dpy, d, e->cgc, cx - 6, cy - 6, cx - 2, cy - 6);
        XDrawLine(dpy, d, e->cgc, cx + 6, cy + 2, cx + 6, cy + 6);
        XDrawLine(dpy, d, e->cgc, cx + 6, cy + 6, cx + 2, cy + 6);
        XSetLineAttributes(dpy, e->cgc, 1, LineSolid, CapButt, JoinMiter);
        XDrawRectangle(dpy, d, e->cgc, cx - 5, cy - 5, 10, 10);
        break;
    }
    default:
        break;
    }
}

/* --- панель */

static int ui_button(editor_t *e, int id, int x, int y, int w, int h) {
    btn_t *b = &e->btns[e->nbtn++];
    b->id = id;
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    return x + w;
}

#define ICON_SZ 16
#define ICON_BTN 34
#define TOOL_PAD_ICON 6

static int tool_width(const char *label) {
    return 8 + ICON_SZ + TOOL_PAD_ICON + str_width(13, label) + 10;
}

/* сколько места просит панель — меньше этого окно делать нельзя */
static int panel_width(editor_t *e) {
    int w = 8;
    w += ICON_BTN + 10;                        /* отменить */
    w += tool_width("перо") + 4;               /* перо */
    w += tool_width("текст") + 4;              /* текст */
    w += tool_width("вырезать") + 10;          /* вырезать */
    w += NCOLORS * (26 + 4) + 6;               /* цвета */
    w += 26 + 4 + str_width(13, "толщина 10.5%") + 10 + 4 + 26;
    w += 10 + ICON_BTN + 6 + ICON_BTN + 8;     /* буфер + готово */
    (void)e;
    return w;
}

static void editor_relayout(editor_t *e) {
    e->nbtn = 0;
    int x = 8, y = 6, h = PANEL_H - 12;

    x = ui_button(e, B_UNDO, x, y, ICON_BTN, h);
    x += 10;
    x = ui_button(e, B_PEN, x, y, tool_width("перо"), h);
    x += 4;
    x = ui_button(e, B_TEXT, x, y, tool_width("текст"), h);
    x += 4;
    x = ui_button(e, B_CROP, x, y, tool_width("вырезать"), h);
    x += 10;

    for (int i = 0; i < NCOLORS; i++) {
        x = ui_button(e, B_COLOR0 + i, x, y, 26, h);
        x += 4;
    }
    x += 6;

    x = ui_button(e, B_SHRINK, x, y, 26, h);
    int wlab = str_width(13, "толщина 10.5% ") + 10;
    x += 4 + wlab + 4;
    ui_button(e, B_GROW, x, y, 26, h);
}

static int editor_hit(editor_t *e, int x, int y) {
    for (int i = 0; i < e->nbtn; i++) {
        btn_t *b = &e->btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
            return b->id;
    }
    return 0;
}

static void button_frame(editor_t *e, btn_t *b, int active) {
    uint32_t bg = active ? 0xc9d8f2 : 0xffffff;
    uint32_t bd = active ? 0x4a72d0 : 0xc9c9c4;
    if (e->hot == b->id && !active)
        bg = 0xf1f4fa;
    XSetForeground(dpy, e->cgc, bd);
    XFillRectangle(dpy, e->win, e->cgc, b->x, b->y, b->w, b->h);
    XSetForeground(dpy, e->cgc, bg);
    XFillRectangle(dpy, e->win, e->cgc, b->x + 1, b->y + 1, b->w - 2, b->h - 2);
}

/* кнопка-иконка: label == NULL — только иконка по центру */
static void editor_draw_button(editor_t *e, btn_t *b, int icon, const char *label,
                               int active, int enabled) {
    button_frame(e, b, active);
    int cy = b->y + b->h / 2;
    uint32_t col = enabled ? 0x202020 : 0xa8a8a4;
    if (label) {
        int tw = str_width(13, label);
        int total = ICON_SZ + TOOL_PAD_ICON + tw;
        int x0 = b->x + (b->w - total) / 2;
        draw_icon(e, e->win, icon, x0 + ICON_SZ / 2, cy, ICON_SZ, col);
        draw_str(e->draw, col, 13, x0 + ICON_SZ + TOOL_PAD_ICON,
                 b->y + (b->h - font_get(13)->ascent - font_get(13)->descent) / 2, label);
    } else {
        draw_icon(e, e->win, icon, b->x + b->w / 2, cy, ICON_SZ, col);
    }
}

static void editor_draw_swatch(editor_t *e, btn_t *b, uint32_t col, int active) {
    XSetForeground(dpy, e->cgc, active ? 0x202020 : 0xbdbdb8);
    XFillRectangle(dpy, e->win, e->cgc, b->x, b->y, b->w, b->h);
    int pad = active ? 3 : 2;
    XSetForeground(dpy, e->cgc, col);
    XFillRectangle(dpy, e->win, e->cgc, b->x + pad, b->y + pad, b->w - 2 * pad,
                   b->h - 2 * pad);
}

static void editor_paint(editor_t *e) {
    XSetForeground(dpy, e->cgc, 0xf2f2ee);
    XFillRectangle(dpy, e->win, e->cgc, 0, 0, e->cw, PANEL_H);
    XSetForeground(dpy, e->cgc, 0xc9c9c4);
    XFillRectangle(dpy, e->win, e->cgc, 0, PANEL_H - 1, e->cw, 1);

    editor_relayout(e);

    /* правые кнопки: считаем позиции до отрисовки */
    btn_t *bcopy = NULL, *bdone = NULL;
    for (int i = 0; i < e->nbtn; i++) {
        if (e->btns[i].id == B_COPY)
            bcopy = &e->btns[i];
        if (e->btns[i].id == B_DONE)
            bdone = &e->btns[i];
    }
    if (!bcopy)
        bcopy = &e->btns[e->nbtn++];
    if (!bdone)
        bdone = &e->btns[e->nbtn++];
    bcopy->id = B_COPY;
    bdone->id = B_DONE;
    bcopy->y = bdone->y = 6;
    bcopy->h = bdone->h = PANEL_H - 12;
    bdone->w = bcopy->w = ICON_BTN;
    bdone->x = e->cw - bdone->w - 8;
    bcopy->x = bdone->x - bcopy->w - 6;

    /* подпись размера между − и + */
    int lab_x = 0, lab_w = 0;
    for (int i = 0; i < e->nbtn; i++) {
        if (e->btns[i].id == B_SHRINK)
            lab_x = e->btns[i].x + e->btns[i].w;
        if (e->btns[i].id == B_GROW)
            lab_w = e->btns[i].x - lab_x;
    }
    char lbl[64];
    if (e->cfg.tool == 2)
        snprintf(lbl, sizeof(lbl), "рамка");
    else if (e->cfg.tool == 1)
        snprintf(lbl, sizeof(lbl), "кегль %.1f%%", e->cfg.txt);
    else
        snprintf(lbl, sizeof(lbl), "толщина %.1f%%", e->cfg.pen);
    int tw = str_width(13, lbl);
    draw_str(e->draw, 0x202020, 13, lab_x + (lab_w - tw) / 2,
             (PANEL_H - font_get(13)->ascent - font_get(13)->descent) / 2, lbl);

    for (int i = 0; i < e->nbtn; i++) {
        btn_t *b = &e->btns[i];
        switch (b->id) {
        case B_UNDO:
            editor_draw_button(e, b, ICON_UNDO, NULL, 0, e->ops.n > 0);
            break;
        case B_COPY:
            editor_draw_button(e, b, ICON_COPY, NULL, 0, 1);
            break;
        case B_DONE:
            editor_draw_button(e, b, ICON_CHECK, NULL, 0, 1);
            break;
        case B_PEN:
            editor_draw_button(e, b, ICON_PEN, "перо", e->cfg.tool == 0, 1);
            break;
        case B_TEXT:
            editor_draw_button(e, b, ICON_TEXT, "текст", e->cfg.tool == 1, 1);
            break;
        case B_CROP:
            editor_draw_button(e, b, ICON_CROP, "вырезать", e->cfg.tool == 2, 1);
            break;
        case B_SHRINK:
            editor_draw_button(e, b, ICON_NONE, "−", 0, 1);
            break;
        case B_GROW:
            editor_draw_button(e, b, ICON_NONE, "+", 0, 1);
            break;
        default:
            if (b->id >= B_COLOR0 && b->id < B_COLOR0 + NCOLORS) {
                int idx = b->id - B_COLOR0;
                uint32_t c;
                parse_color(e->cfg.colors[idx], &c);
                editor_draw_swatch(e, b, c, c == e->cfg.color);
            }
            break;
        }
    }
    if (getenv("XSNIP_DEBUG"))
        for (int i = 0; i < e->nbtn; i++)
            fprintf(stderr, "btn id=%d x=%d w=%d\n", e->btns[i].id, e->btns[i].x,
                    e->btns[i].w);
}

static void editor_blit_canvas(editor_t *e) {
    if (!e->view_xi)
        return;
    XPutImage(dpy, e->win, e->cgc, e->view_xi, 0, 0, 0, PANEL_H, e->cw, e->ch);
    editor_draw_ops(e, e->win, e->draw, 1);
    if (e->crop_active) {
        int x0 = e->cx0 < e->cx1 ? e->cx0 : e->cx1;
        int y0 = e->cy0 < e->cy1 ? e->cy0 : e->cy1;
        int w = (e->cx0 < e->cx1 ? e->cx1 - e->cx0 : e->cx0 - e->cx1);
        int h = (e->cy0 < e->cy1 ? e->cy1 - e->cy0 : e->cy0 - e->cy1);
        int sx = e->ox + (int)(x0 * e->scale);
        int sy = PANEL_H + e->oy + (int)(y0 * e->scale);
        int sw = (int)(w * e->scale);
        int sh = (int)(h * e->scale);
        XSetForeground(dpy, e->cgc, 0x000000);
        XSetLineAttributes(dpy, e->cgc, 1, LineSolid, CapButt, JoinMiter);
        XDrawRectangle(dpy, e->win, e->cgc, sx - 1, sy - 1, sw + 2, sh + 2);
        XDrawRectangle(dpy, e->win, e->cgc, sx + 1, sy + 1, sw - 2, sh - 2);
        XSetForeground(dpy, e->cgc, 0xffffff);
        XDrawRectangle(dpy, e->win, e->cgc, sx, sy, sw, sh);
        char lab[64];
        snprintf(lab, sizeof(lab), "%d x %d", w, h);
        int ty = (sy - PANEL_H > 20) ? sy + 3 : sy + sh - 16;
        int tw = str_width(13, lab);
        XSetForeground(dpy, e->cgc, 0x000000);
        XFillRectangle(dpy, e->win, e->cgc, sx + 3, ty - 1, tw + 6, 17);
        draw_str(e->draw, 0xffffff, 13, sx + 6, ty, lab);
    }

    if (e->text_active) {
        char line[TEXT_MAX + 32];
        snprintf(line, sizeof(line), "текст: %s_", e->textbuf);
        int px = 14;
        int w = str_width(px, line) + 20;
        int h = font_get(px)->ascent + font_get(px)->descent + 12;
        int x = 12, y = PANEL_H + e->ch - h - 12;
        XSetForeground(dpy, e->cgc, 0x202020);
        XFillRectangle(dpy, e->win, e->cgc, x, y, w, h);
        XSetForeground(dpy, e->cgc, 0xffffff);
        XFillRectangle(dpy, e->win, e->cgc, x + 1, y + 1, w - 2, h - 2);
        XSetForeground(dpy, e->cgc, 0x202020);
        XDrawRectangle(dpy, e->win, e->cgc, x, y, w - 1, h - 1);
        draw_str(e->draw, 0x101010, px, x + 10, y + 6, line);
        (void)e->text_wi;
        /* где именно начнётся текст: вертикальная черта высотой с кегль */
        int cx = e->ox + (int)(e->text_x * e->scale);
        int cy = PANEL_H + e->oy + (int)(e->text_y * e->scale);
        int ch = (int)((e->cfg.txt / 100.0 * e->basis) * e->scale + 0.5);
        if (ch < 5)
            ch = 5;
        XSetForeground(dpy, e->cgc, e->cfg.color);
        XFillRectangle(dpy, e->win, e->cgc, cx, cy, 2, ch);
        XSetForeground(dpy, e->cgc, e->cfg.color & 0x7f7f7f);
        XFillRectangle(dpy, e->win, e->cgc, cx, cy + ch, (int)(20 * e->scale) + 4, 1);
    }
}

static void editor_repaint(editor_t *e) {
    editor_paint(e);
    editor_blit_canvas(e);
}

static void editor_rebuild(editor_t *e) {
    view_rebuild(e);
}

/* --- действия */

static void editor_cursor_for(editor_t *e, int y) {
    int zone = (y < PANEL_H) ? 0 : 1;
    if (zone == e->cursor_zone)
        return;
    e->cursor_zone = zone;
    Cursor c;
    if (zone == 0)
        c = e->cur_arrow;
    else
        c = e->cfg.tool ? e->cur_text : e->cur_pen;
    XDefineCursor(dpy, e->win, c);
    XFlush(dpy);
}

static void editor_set_tool(editor_t *e, int tool) {
    if (e->cfg.tool == tool)
        return;
    e->cfg.tool = tool;
    if (e->cur_arrow == 0)
        return;
    Cursor c = e->cursor_zone == 0 ? e->cur_arrow
                                   : (tool == 1 ? e->cur_text : e->cur_pen);
    XDefineCursor(dpy, e->win, c);
    cfg_save(&e->cfg);
    editor_paint(e);
    XFlush(dpy);
}

static void editor_adjust(editor_t *e, int dir) {
    if (e->cfg.tool == 2)
        return;
    if (e->cfg.tool) {
        e->cfg.txt += dir * TXT_STEP;
        if (e->cfg.txt < TXT_MIN) e->cfg.txt = TXT_MIN;
        if (e->cfg.txt > TXT_MAX) e->cfg.txt = TXT_MAX;
    } else {
        e->cfg.pen += dir * PEN_STEP;
        if (e->cfg.pen < PEN_MIN) e->cfg.pen = PEN_MIN;
        if (e->cfg.pen > PEN_MAX) e->cfg.pen = PEN_MAX;
    }
    cfg_save(&e->cfg);
    editor_paint(e);
    XFlush(dpy);
}

/* --- отрисовка операций в память (pixmap) */

static img_t *editor_flatten(editor_t *e) {
    Pixmap pm = XCreatePixmap(dpy, root, e->iw, e->ih, depth);
    GC g = XCreateGC(dpy, pm, 0, NULL);
    XImage *xi = img_to_ximage(e->src);
    XPutImage(dpy, pm, g, xi, 0, 0, 0, 0, e->iw, e->ih);
    XDestroyImage(xi);

    XftDraw *xd = XftDrawCreate(dpy, pm, visual, cmap);
    for (int i = 0; i < e->ops.n; i++) {
        op_t *op = &e->ops.items[i];
        if (op->kind == OP_STROKE) {
            int lw = (int)(op->width + 0.5);
            if (lw < 1)
                lw = 1;
            XSetForeground(dpy, g, op->color);
            XSetLineAttributes(dpy, g, lw, LineSolid, CapRound, JoinRound);
            if (op->n == 1)
                XFillArc(dpy, pm, g, op->pts[0] - lw / 2, op->pts[1] - lw / 2, lw, lw, 0,
                         360 * 64);
            else
                for (int k = 1; k < op->n; k++)
                    XDrawLine(dpy, pm, g, op->pts[(k - 1) * 2], op->pts[(k - 1) * 2 + 1],
                              op->pts[k * 2], op->pts[k * 2 + 1]);
        } else if (op->kind == OP_TEXT && op->text) {
            draw_str(xd, op->color, op->size, op->tx, op->ty, op->text);
        }
    }
    XSync(dpy, False);
    XImage *out_xi = XGetImage(dpy, pm, 0, 0, e->iw, e->ih, AllPlanes, ZPixmap);
    img_t *out = img_new(e->iw, e->ih);
    for (int y = 0; y < e->ih; y++)
        for (int x = 0; x < e->iw; x++)
            out->px[(size_t)y * e->iw + x] = pixel_from_ximage(out_xi, x, y);
    XDestroyImage(out_xi);
    XftDrawDestroy(xd);
    XFreeGC(dpy, g);
    XFreePixmap(dpy, pm);
    return out;
}

static void editor_copy(editor_t *e) {
    if (getenv("XSNIP_DEBUG"))
        fprintf(stderr, "copy: ops=%d\n", e->ops.n);
    img_t *flat = editor_flatten(e);
    unsigned char *png;
    size_t len;
    if (png_encode(flat, &png, &len) == 0) {
        clipboard_put_png(png, len);
        free(png);
    }
    img_free(flat);
}

static void editor_done(editor_t *e) {
    if (e->outpath) {
        img_t *flat = editor_flatten(e);
        if (png_write_file(flat, e->outpath) == 0)
            e->saved = 1;
        img_free(flat);
    } else {
        editor_copy(e);
    }
    e->done = 1;
}

static void editor_start_text(editor_t *e, int wx, int wy) {
    if (wy < PANEL_H)
        return;
    e->text_active = 1;
    e->text_len = 0;
    e->textbuf[0] = 0;
    e->text_x = (int)((wx - e->ox) / e->scale);
    e->text_y = (int)((wy - PANEL_H - e->oy) / e->scale);
}

static void editor_commit_text(editor_t *e) {
    if (getenv("XSNIP_DEBUG"))
        fprintf(stderr, "commit '%s' len=%d at %d,%d\n", e->textbuf, e->text_len,
                e->text_x, e->text_y);
    if (e->text_len > 0) {
        op_t *op = ops_push(&e->ops);
        op->kind = OP_TEXT;
        op->color = e->cfg.color;
        op->size = (int)(e->cfg.txt / 100.0 * e->basis + 0.5);
        if (op->size < 4)
            op->size = 4;
        op->text = strdup(e->textbuf);
        op->tx = e->text_x;
        op->ty = e->text_y;
    }
    e->text_active = 0;
    e->text_len = 0;
    e->textbuf[0] = 0;
    editor_blit_canvas(e);
    XFlush(dpy);
}

/* вырезать прямоугольник из картинки и продолжить редактирование внутри */
static void editor_crop_apply(editor_t *e) {
    int x0 = e->cx0 < e->cx1 ? e->cx0 : e->cx1;
    int y0 = e->cy0 < e->cy1 ? e->cy0 : e->cy1;
    int w = e->cx0 < e->cx1 ? e->cx1 - e->cx0 : e->cx0 - e->cx1;
    int h = e->cy0 < e->cy1 ? e->cy1 - e->cy0 : e->cy0 - e->cy1;
    e->crop_active = 0;

    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > e->iw) w = e->iw - x0;
    if (y0 + h > e->ih) h = e->ih - y0;
    if (w < 5 || h < 5) {
        editor_blit_canvas(e);
        XFlush(dpy);
        return;
    }

    img_t *c = img_crop(e->src, x0, y0, w, h);
    img_free(e->src);
    e->src = c;
    e->iw = c->w;
    e->ih = c->h;
    e->basis = e->iw < e->ih ? e->iw : e->ih;

    for (int i = 0; i < e->ops.n; i++) {
        op_t *op = &e->ops.items[i];
        if (op->kind == OP_STROKE)
            for (int k = 0; k < op->n; k++) {
                op->pts[k * 2] -= x0;
                op->pts[k * 2 + 1] -= y0;
            }
        else {
            op->tx -= x0;
            op->ty -= y0;
        }
    }

    view_rebuild(e);
    editor_repaint(e);
    editor_set_tool(e, 0);   /* после обрезки — снова перо */
    XFlush(dpy);
}

/* --- основное окно */

/* XIC обязан ссылаться на своё окно, иначе XIM отдаёт только keysym без символов */
static void editor_make_ic(Window win) {
    if (!xim)
        return;
    if (xic_editor) {
        XDestroyIC(xic_editor);
        xic_editor = NULL;
    }
    xic_editor = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, win, XNFocusWindow, win, NULL);
    if (xic_editor)
        XSetICFocus(xic_editor);
}

static void editor_map_prepare(editor_t *e) {
    XClassHint ch = {(char *)"xsnip", (char *)"Xsnip"};
    XSetClassHint(dpy, e->win, &ch);
    XStoreName(dpy, e->win, "xsnip");
    XSetCommand(dpy, e->win, (char *[]){(char *)"xsnip", NULL}, 1);

    Atom wt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom val = XInternAtom(dpy,
                           e->cfg.wintype ? "_NET_WM_WINDOW_TYPE_NORMAL"
                                          : "_NET_WM_WINDOW_TYPE_DIALOG",
                           False);
    XChangeProperty(dpy, e->win, wt, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&val, 1);

    XWMHints wmh = {0};
    wmh.flags = InputHint | StateHint;
    wmh.input = True;
    wmh.initial_state = NormalState;
    XSetWMHints(dpy, e->win, &wmh);

    e->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, e->win, &e->wm_delete, 1);
}

static void editor_place(editor_t *e, int winw, int winh) {
    int mx = e->mon.x, my = e->mon.y, W = e->mon.w, H = e->mon.h;
    int x = mx, y = my;
    switch (e->cfg.pos) {
    case 0: x = mx + (W - winw) / 2; y = my + (H - winh) / 2; break;
    case 1: x = mx + MARGIN; y = my + MARGIN; break;
    case 2: x = mx + W - winw - MARGIN; y = my + MARGIN; break;
    case 3: x = mx + MARGIN; y = my + H - winh - MARGIN; break;
    case 4: x = mx + W - winw - MARGIN; y = my + H - winh - MARGIN; break;
    }
    if (x < mx) x = mx;
    if (y < my) y = my;

    XSizeHints hints = {0};
    hints.flags = USPosition | PPosition;
    hints.x = x;
    hints.y = y;
    XSetWMNormalHints(dpy, e->win, &hints);
    XMoveWindow(dpy, e->win, x, y);
}

static void editor_focus(editor_t *e) {
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, e->win, &wa) && wa.map_state == IsViewable)
        XSetInputFocus(dpy, e->win, RevertToPointerRoot, CurrentTime);
}

static void editor_run(editor_t *e) {
    /* никогда не больше экрана, на котором живём */
    int maxw = e->mon.w * 95 / 100;
    int maxh = e->mon.h * 85 / 100;
    int winw = e->iw < maxw ? e->iw : maxw;
    int winh = e->ih + PANEL_H < maxh ? e->ih + PANEL_H : maxh;
    int needed = panel_width(e);
    if (needed > e->mon.w - 2 * MARGIN)
        needed = e->mon.w - 2 * MARGIN;
    if (winw < needed)
        winw = needed;
    if (winw > e->mon.w)
        winw = e->mon.w;
    if (winh > e->mon.h)
        winh = e->mon.h;

    e->win = XCreateSimpleWindow(dpy, root, 0, 0, winw, winh, 0, 0, 0xf2f2ee);
    if (!e->win)
        die("не удалось создать окно редактора");
    XSelectInput(dpy, e->win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | StructureNotifyMask |
                     ButtonMotionMask);
    editor_map_prepare(e);
    e->draw = XftDrawCreate(dpy, e->win, visual, cmap);
    e->cgc = XCreateGC(dpy, e->win, 0, NULL);
    e->cur_pen = XCreateFontCursor(dpy, XC_crosshair);
    e->cur_text = XCreateFontCursor(dpy, XC_xterm);
    e->cur_arrow = XCreateFontCursor(dpy, XC_left_ptr);
    e->cursor_zone = 1;
    XDefineCursor(dpy, e->win, e->cfg.tool ? e->cur_text : e->cur_pen);

    XMapWindow(dpy, e->win);
    XSync(dpy, False);
    editor_focus(e);
    editor_place(e, winw, winh);
    editor_make_ic(e->win);

    e->cw = winw;
    e->ch = winh - PANEL_H;
    editor_rebuild(e);
    editor_repaint(e);
    XFlush(dpy);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.window == e->win && ev.xexpose.count == 0) {
                editor_repaint(e);
                editor_focus(e);
            }
            break;
        case ConfigureNotify:
            if (ev.xconfigure.window == e->win) {
                int nw = ev.xconfigure.width, nh = ev.xconfigure.height;
                if (nw != e->cw || nh - PANEL_H != e->ch) {
                    e->cw = nw;
                    e->ch = nh - PANEL_H;
                    if (e->ch < 2)
                        e->ch = 2;
                    editor_rebuild(e);
                    editor_repaint(e);
                }
            }
            break;
        case MotionNotify: {
            int x = ev.xmotion.x, y = ev.xmotion.y;
            if (e->crop_active) {
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - PANEL_H - e->oy) / e->scale);
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                if (ix > e->iw) ix = e->iw;
                if (iy > e->ih) iy = e->ih;
                e->cx1 = ix;
                e->cy1 = iy;
                editor_blit_canvas(e);
                XFlush(dpy);
                break;
            }
            if (e->drawing) {
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - PANEL_H - e->oy) / e->scale);
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                if (ix > e->iw) ix = e->iw;
                if (iy > e->ih) iy = e->ih;
                if (e->cur_n == e->cur_cap) {
                    e->cur_cap = e->cur_cap ? e->cur_cap * 2 : 256;
                    e->cur_pts = realloc(e->cur_pts, (size_t)e->cur_cap * 2 * sizeof(int));
                    if (!e->cur_pts)
                        die("нет памяти (штрих)");
                }
                e->cur_pts[e->cur_n * 2] = ix;
                e->cur_pts[e->cur_n * 2 + 1] = iy;
                e->cur_n++;
                int lw = (int)(e->cur_w * e->scale + 0.5);
                if (lw < 1) lw = 1;
                XSetForeground(dpy, e->cgc, e->cur_color);
                XSetLineAttributes(dpy, e->cgc, lw, LineSolid, CapRound, JoinRound);
                if (e->cur_n >= 2) {
                    int x0 = e->ox + (int)(e->cur_pts[(e->cur_n - 2) * 2] * e->scale);
                    int y0 = PANEL_H + e->oy +
                             (int)(e->cur_pts[(e->cur_n - 2) * 2 + 1] * e->scale);
                    int x1 = e->ox + (int)(ix * e->scale);
                    int y1 = PANEL_H + e->oy + (int)(iy * e->scale);
                    XDrawLine(dpy, e->win, e->cgc, x0, y0, x1, y1);
                }
                XFlush(dpy);
            } else {
                editor_cursor_for(e, y);
                int hit = editor_hit(e, x, y);
                if (hit != e->hot) {
                    e->hot = hit;
                    editor_paint(e);
                    XFlush(dpy);
                }
            }
            break;
        }
        case ButtonPress: {
            int x = ev.xbutton.x, y = ev.xbutton.y;
            editor_focus(e);
            if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
                editor_adjust(e, ev.xbutton.button == Button4 ? 1 : -1);
                break;
            }
            if (ev.xbutton.button == Button3) {
                if (e->text_active) {
                    e->text_active = 0;
                    editor_blit_canvas(e);
                }
                break;
            }
            if (ev.xbutton.button != Button1)
                break;
            if (y < PANEL_H) {
                int id = editor_hit(e, x, y);
                if (id == B_COPY) {
                    editor_copy(e);
                } else if (id == B_UNDO) {
                    if (e->ops.n > 0) {
                        op_t *op = &e->ops.items[e->ops.n - 1];
                        free(op->pts);
                        free(op->text);
                        e->ops.n--;
                        editor_blit_canvas(e);
                    }
                } else if (id == B_PEN) {
                    editor_set_tool(e, 0);
                } else if (id == B_TEXT) {
                    editor_set_tool(e, 1);
                } else if (id == B_CROP) {
                    editor_set_tool(e, 2);
                } else if (id == B_SHRINK) {
                    editor_adjust(e, -1);
                } else if (id == B_GROW) {
                    editor_adjust(e, 1);
                } else if (id >= B_COLOR0 && id < B_COLOR0 + NCOLORS) {
                    uint32_t c;
                    if (parse_color(e->cfg.colors[id - B_COLOR0], &c)) {
                        e->cfg.color = c;
                        cfg_save(&e->cfg);
                        editor_paint(e);
                    }
                } else if (id == B_DONE) {
                    editor_done(e);
                    if (e->done)
                        goto finished;
                }
                XFlush(dpy);
                break;
            }
            if (e->cfg.tool == 2) {
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - PANEL_H - e->oy) / e->scale);
                e->cx0 = e->cx1 = ix;
                e->cy0 = e->cy1 = iy;
                e->crop_active = 1;
                editor_blit_canvas(e);
                XFlush(dpy);
                break;
            }
            if (e->cfg.tool == 0) {
                e->drawing = 1;
                e->cur_n = 0;
                e->cur_w = e->cfg.pen / 100.0 * e->basis;
                if (e->cur_w < 1)
                    e->cur_w = 1;
                e->cur_color = e->cfg.color;
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - PANEL_H - e->oy) / e->scale);
                if (e->cur_cap < 2) {
                    e->cur_cap = 256;
                    e->cur_pts = realloc(e->cur_pts, (size_t)e->cur_cap * 2 * sizeof(int));
                }
                e->cur_pts[0] = ix;
                e->cur_pts[1] = iy;
                e->cur_n = 1;
            } else {
                editor_start_text(e, x, y);
                editor_blit_canvas(e);
                XFlush(dpy);
            }
            break;
        }
        case ButtonRelease: {
            if (ev.xbutton.button != Button1)
                break;
            if (e->crop_active) {
                editor_crop_apply(e);
                break;
            }
            if (getenv("XSNIP_DEBUG"))
                fprintf(stderr, "release button=%d drawing=%d cur_n=%d\n", ev.xbutton.button,
                        e->drawing, e->cur_n);
            if (e->drawing) {
                e->drawing = 0;
                if (e->cur_n > 0) {
                    op_t *op = ops_push(&e->ops);
                    op->kind = OP_STROKE;
                    op->color = e->cur_color;
                    op->width = e->cur_w;
                    op->n = e->cur_n;
                    op->pts = malloc((size_t)e->cur_n * 2 * sizeof(int));
                    if (!op->pts)
                        die("нет памяти (штрих)");
                    memcpy(op->pts, e->cur_pts, (size_t)e->cur_n * 2 * sizeof(int));
                }
                editor_blit_canvas(e);
                XFlush(dpy);
            }
            break;
        }
        case KeyPress: {
            KeySym ks = 0;
            char buf[64];
            if (getenv("XSNIP_DEBUG"))
                fprintf(stderr, "keypress state=0x%x text_active=%d\n", ev.xkey.state,
                        e->text_active);
            if (e->text_active) {
                int n = 0;
                if (xic_editor) {
                    Status st = 0;
                    n = Xutf8LookupString(xic_editor, &ev.xkey, buf, sizeof(buf) - 1, &ks, &st);
                    if (getenv("XSNIP_DEBUG"))
                        fprintf(stderr, "  xim: st=%d n=%d\n", st, n);
                    if (!(st & (XLookupChars | XLookupBoth)))
                        n = 0;
                } else {
                    n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
                }
                if (ks == XK_Return || ks == XK_KP_Enter) {
                    editor_commit_text(e);
                    break;
                }
                if (ks == XK_Escape) {
                    e->text_active = 0;
                    e->text_len = 0;
                    e->textbuf[0] = 0;
                    editor_blit_canvas(e);
                    XFlush(dpy);
                    break;
                }
                if (ks == XK_BackSpace) {
                    if (e->text_len > 0) {
                        e->text_len--;
                        e->textbuf[e->text_len] = 0;
                        editor_blit_canvas(e);
                        XFlush(dpy);
                    }
                    break;
                }
                if (getenv("XSNIP_DEBUG"))
                    fprintf(stderr, "  ks=0x%lx n=%d buf='%.*s'\n", ks, n, n > 0 ? n : 0, buf);
                if (n > 0) {
                    if (e->text_len + n < TEXT_MAX - 1) {
                        memcpy(e->textbuf + e->text_len, buf, (size_t)n);
                        e->text_len += n;
                        e->textbuf[e->text_len] = 0;
                        if (getenv("XSNIP_DEBUG"))
                            fprintf(stderr, "  textbuf='%s'\n", e->textbuf);
                        editor_blit_canvas(e);
                        XFlush(dpy);
                    }
                }
                break;
            }
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            int ctrl = (ev.xkey.state & ControlMask) != 0;
            if (ctrl && (ks == XK_z || ks == XK_Z)) {
                if (e->ops.n > 0) {
                    op_t *op = &e->ops.items[e->ops.n - 1];
                    free(op->pts);
                    free(op->text);
                    e->ops.n--;
                    editor_blit_canvas(e);
                    XFlush(dpy);
                }
                break;
            }
            if (ks == XK_Escape) {
                e->done = 1;
                goto finished;
            }
            if (ks == XK_1) {
                editor_set_tool(e, 0);
            } else if (ks == XK_2) {
                editor_set_tool(e, 1);
            } else if (ks == XK_3) {
                editor_set_tool(e, 2);
            } else if (ks >= XK_4 && ks < XK_4 + NCOLORS) {
                uint32_t c;
                if (parse_color(e->cfg.colors[ks - XK_4], &c)) {
                    e->cfg.color = c;
                    cfg_save(&e->cfg);
                    editor_paint(e);
                    XFlush(dpy);
                }
            }
            break;
        }
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == e->wm_delete) {
                e->done = 1;
                goto finished;
            }
            break;
        default:
            break;
        }
    }
finished:
    cfg_save(&e->cfg);
    XDefineCursor(dpy, e->win, e->cur_arrow);
    if (e->view_xi) {
        XDestroyImage(e->view_xi);
        e->view_xi = NULL;
    }
    img_free(e->view);
    XftDrawDestroy(e->draw);
    XFreeGC(dpy, e->cgc);
    XFreeCursor(dpy, e->cur_pen);
    XFreeCursor(dpy, e->cur_text);
    XFreeCursor(dpy, e->cur_arrow);
    XDestroyWindow(dpy, e->win);
    free(e->cur_pts);
}

/* -------------------------------------------------------------------- CLI */

static void usage(FILE *f) {
    fprintf(f,
            "использование: xsnip [опции]\n"
            "  -s, --select             выбрать область мышью\n"
            "  -a, --all                весь виртуальный экран (все мониторы)\n"
            "  -o, --output FILE        сохранить в FILE (иначе — в буфер)\n"
            "  -d, --delay SEC          задержка перед снимком\n"
            "      --colors c1,c2,c3,c4,c5  палитра (#rrggbb)\n"
            "      --color N|#rrggbb    активный цвет\n"
            "      --pen PCT            толщина пера в %% от min(w,h) [0.1..5]\n"
            "      --text PCT           кегль текста в %% от min(w,h) [1..50]\n"
            "      --window dialog|normal  тип окна редактора\n"
            "      --pos center|tl|tr|bl|br  положение окна\n"
            "  -h, --help               эта справка\n");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");

    cfg_t cfg;
    cfg_defaults(&cfg);
    cfg_load(&cfg);

    int select_flag = 0;
    int all_screens = 0;
    const char *outpath = NULL;
    double delay = 0.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-s") || !strcmp(a, "--select")) {
            select_flag = 1;
        } else if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
            all_screens = 1;
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (++i >= argc)
                die("нужно имя файла после %s", a);
            outpath = argv[i];
        } else if (!strncmp(a, "--output=", 9)) {
            outpath = a + 9;
        } else if (!strcmp(a, "-d") || !strcmp(a, "--delay")) {
            if (++i >= argc)
                die("нужна задержка после %s", a);
            delay = atof(argv[i]);
        } else if (!strncmp(a, "--delay=", 8)) {
            delay = atof(a + 8);
        } else if (!strcmp(a, "--colors")) {
            if (++i >= argc)
                die("нужен список цветов");
            char *list = strdup(argv[i]);
            char *save = NULL;
            int k = 0;
            for (char *t = strtok_r(list, ",", &save); t && k < NCOLORS;
                 t = strtok_r(NULL, ",", &save)) {
                uint32_t c;
                if (!parse_color(t, &c))
                    die("плохой цвет: %s", t);
                snprintf(cfg.colors[k], sizeof(cfg.colors[k]), "%s", t);
                k++;
            }
            free(list);
            if (k == 0)
                die("пустой список цветов");
            cfg.color = 0;
            parse_color(cfg.colors[0], &cfg.color);
        } else if (!strcmp(a, "--color")) {
            if (++i >= argc)
                die("нужен цвет");
            int n = atoi(argv[i]);
            if (n >= 1 && n <= NCOLORS) {
                parse_color(cfg.colors[n - 1], &cfg.color);
            } else if (!parse_color(argv[i], &cfg.color)) {
                die("плохой цвет: %s", argv[i]);
            }
        } else if (!strcmp(a, "--pen")) {
            if (++i >= argc)
                die("нужна толщина");
            cfg.pen = atof(argv[i]);
        } else if (!strcmp(a, "--text")) {
            if (++i >= argc)
                die("нужен кегль");
            cfg.txt = atof(argv[i]);
        } else if (!strcmp(a, "--window")) {
            if (++i >= argc)
                die("нужен тип окна");
            if (!strcmp(argv[i], "dialog"))
                cfg.wintype = 0;
            else if (!strcmp(argv[i], "normal"))
                cfg.wintype = 1;
            else
                die("неизвестный тип окна: %s", argv[i]);
        } else if (!strcmp(a, "--pos")) {
            if (++i >= argc)
                die("нужна позиция");
            int found = -1;
            for (int k = 0; k < 5; k++)
                if (!strcmp(argv[i], POS_NAMES[k]))
                    found = k;
            if (found < 0)
                die("неизвестная позиция: %s", argv[i]);
            cfg.pos = found;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("xsnip 2.0 (C, Xlib+Xft+zlib)\n");
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (cfg.pen < PEN_MIN) cfg.pen = PEN_MIN;
    if (cfg.pen > PEN_MAX) cfg.pen = PEN_MAX;
    if (cfg.txt < TXT_MIN) cfg.txt = TXT_MIN;
    if (cfg.txt > TXT_MAX) cfg.txt = TXT_MAX;

    XSetErrorHandler(x_error_handler);
    dpy = XOpenDisplay(NULL);
    if (!dpy)
        die("нет дисплея (проверь $DISPLAY)");
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    visual = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    gc = DefaultGC(dpy, scr);

    monitors_init();

    A_CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
    A_TARGETS = XInternAtom(dpy, "TARGETS", False);
    A_INCR = XInternAtom(dpy, "INCR", False);
    A_PNG_IMAGE = XInternAtom(dpy, "image/png", False);
    A_PNG_UPPER = XInternAtom(dpy, "PNG", False);
    A_TIMESTAMP = XInternAtom(dpy, "TIMESTAMP", False);

    xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim) {
        (void)0; /* XIC создаётся под конкретное окно в editor_make_ic() */
    }

    if (delay > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)delay;
        ts.tv_nsec = (long)((delay - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }

    int px = 0, py = 0;
    pointer_pos(&px, &py);
    mon_t mon = monitor_at(px, py);

    img_t *full = capture_root();
    img_t *shot = NULL;

    if (select_flag) {
        rect_t r = select_area(full);
        if (!r.ok) {
            img_free(full);
            XCloseDisplay(dpy);
            return 1;
        }
        shot = img_crop(full, r.x, r.y, r.w, r.h);
        img_free(full);
        /* редактор открываем на том экране, где выделяли */
        mon = monitor_at(r.x + r.w / 2, r.y + r.h / 2);
    } else if (all_screens) {
        shot = full;
        if (getenv("XSNIP_DEBUG"))
            fprintf(stderr, "снимок всего виртуального экрана: %dx%d\n", shot->w, shot->h);
    } else {
        if (getenv("XSNIP_DEBUG"))
            fprintf(stderr, "снимок экрана %dx%d+%d+%d (под курсором)\n", mon.w, mon.h,
                    mon.x, mon.y);
        shot = img_crop(full, mon.x, mon.y, mon.w, mon.h);
        img_free(full);
    }

    editor_t e = {0};
    e.src = shot;
    e.iw = shot->w;
    e.ih = shot->h;
    e.cfg = cfg;
    e.outpath = outpath;
    e.mon = mon;
    e.basis = e.iw < e.ih ? e.iw : e.ih;

    editor_run(&e);

    ops_free(&e.ops);
    /* после «вырезать» e.src уже другая картинка, shot освобождать нельзя дважды */
    img_free(e.src);
    if (xim) {
        if (xic_editor)
            XDestroyIC(xic_editor);
        XCloseIM(xim);
    }
    /* gc здесь — DefaultGC(dpy, scr): освобождать его нельзя, Xlib владеет им */
    XCloseDisplay(dpy);

    if (outpath)
        return e.saved ? 0 : 1;
    return 0;
}
