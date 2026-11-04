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
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xinerama.h>
#include <X11/keysym.h>

#include <fontconfig/fontconfig.h>
#include <zlib.h>

/* ------------------------------------------------------------------ общее */

#define XSNIP_VERSION "3.11"
#define NCOLORS 5
/* Панель и всё, что её составляет, масштабируется от самого высокого монитора:
 * за базу взято 800 px по высоте (как было раньше), дальше линейно. */
static int max_mon_h = 600;   /* самая большая высота среди мониторов */
static int panel_h = 46;

static int max_mon_w = 1000;

static int sp(int base) {
    int by_h = (base * max_mon_h + 400) / 800;
    int by_w = (base * max_mon_w + 500) / 1000;   /* портретный экран: не шире него */
    int v = by_h < by_w ? by_h : by_w;
    if (v < base)
        v = base;
    if (v > base * 4)
        v = base * 4;
    return v;
}
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
    static int reported = 0;
    /* гонки с WM дают BadMatch на XSetInputFocus — их не страшно, а вот
     * BadDrawable/BadValue стоит показать хотя бы несколько раз */
    if (getenv("XSNIP_DEBUG") || (reported < 3 && ev->error_code != BadMatch)) {
        fprintf(stderr, "xsnip: X error opcode=%d code=%d\n", ev->request_code,
                ev->error_code);
        reported++;
    }
    return 0;
}

static Display *dpy;
static int scr;
static Visual *visual;
static Colormap cmap;
static Window root;
static int depth;
static XIM xim;
static XIC xic_editor;


/* ------------------------------------------------------------ локализация */

enum {
    STR_UNDO = 0, STR_PEN, STR_TEXT, STR_CROP, STR_DONE,
    STR_WIDTH, STR_SIZE, STR_FRAME, STR_SHRINK, STR_GROW, STR_INPUT,
    STR_COLOR, STR_USAGE, STR_COUNT
};

static const char *LANG_TABLE[3][STR_COUNT] = {
    /* en — по умолчанию */
    {
        "Undo (Ctrl+Z)",
        "Pen",
        "Text",
        "Crop",
        "Done (save and close)",
        "Thickness",
        "Font size",
        "Frame",
        "Thinner",
        "Thicker",
        "text: ",
        "Colour",
        "usage: xsnip [options]\n"
        "  -s, --select             select a region with the mouse\n"
        "  -o, --output FILE        write FILE instead of copying to clipboard\n"
        "  -d, --delay SEC          wait before the shot\n"
        "  -a, --all                whole virtual screen (all monitors)\n"
        "      --colors c1,..,c5    palette (#rrggbb)\n"
        "      --color N|#rrggbb    active colour\n"
        "      --pen PCT            pen width, %% of min(w,h) [0.1..5]\n"
        "      --text PCT           text size, %% of min(w,h) [1..50]\n"
        "      --window dialog|normal  editor window type\n"
        "      --pos center|tl|tr|bl|br  editor window position\n"
        "  -h, --help               this help\n",
    },
    /* ru */
    {
        "Отменить (Ctrl+Z)",
        "Перо",
        "Текст",
        "Вырезать",
        "Готово — сохранить и закрыть",
        "Толщина",
        "Кегль",
        "Рамка",
        "Тоньше",
        "Толще",
        "текст: ",
        "Цвет",
        "использование: xsnip [опции]\n"
        "  -s, --select             выбрать область мышью\n"
        "  -o, --output FILE        сохранить в FILE (иначе — в буфер)\n"
        "  -d, --delay SEC          задержка перед снимком\n"
        "  -a, --all                весь виртуальный экран (все мониторы)\n"
        "      --colors c1,..,c5    палитра (#rrggbb)\n"
        "      --color N|#rrggbb    активный цвет\n"
        "      --pen PCT            толщина пера в %% от min(w,h) [0.1..5]\n"
        "      --text PCT           кегль текста в %% от min(w,h) [1..50]\n"
        "      --window dialog|normal  тип окна редактора\n"
        "      --pos center|tl|tr|bl|br  положение окна\n"
        "  -h, --help               эта справка\n",
    },
    /* zh */
    {
        "撤销 (Ctrl+Z)",
        "画笔",
        "文字",
        "裁剪",
        "完成 (保存并关闭)",
        "粗细",
        "字号",
        "选框",
        "更细",
        "更粗",
        "文字：",
        "颜色",
        "用法: xsnip [选项]\n"
        "  -s, --select             用鼠标选择区域\n"
        "  -o, --output FILE        保存到 FILE（否则复制到剪贴板）\n"
        "  -d, --delay SEC          截图前等待\n"
        "  -a, --all                整个虚拟屏幕（所有显示器）\n"
        "      --colors c1,..,c5    调色板 (#rrggbb)\n"
        "      --color N|#rrggbb    当前颜色\n"
        "      --pen PCT            画笔粗细，min(w,h) 的百分比 [0.1..5]\n"
        "      --text PCT           文字大小，min(w,h) 的百分比 [1..50]\n"
        "      --window dialog|normal  编辑器窗口类型\n"
        "      --pos center|tl|tr|bl|br  编辑器窗口位置\n"
        "  -h, --help               显示帮助\n",
    },
};

static int lang = 0;   /* 0 en, 1 ru, 2 zh */
static char font_family[128] = "sans";

static const char *t(int id) {
    if (id < 0 || id >= STR_COUNT)
        return "";
    return LANG_TABLE[lang][id];
}

static void lang_init(void) {
    const char *env = getenv("XSNIP_LANG");
    if (!env || !*env)
        env = getenv("LC_ALL");
    if (!env || !*env)
        env = getenv("LC_MESSAGES");
    if (!env || !*env)
        env = getenv("LANG");
    if (!env)
        return;
    char lang_part[8] = {0};
    for (int i = 0; i < 7 && env[i] && env[i] != '_' && env[i] != '.' && env[i] != '-'; i++) {
        lang_part[i] = (char)((env[i] >= 'A' && env[i] <= 'Z') ? env[i] + 32 : env[i]);
    }
    if (!strcmp(lang_part, "ru"))
        lang = 1;
    else if (!strcmp(lang_part, "zh"))
        lang = 2;
    else
        lang = 0;
}

/* ---------------------------------------------------------------- мониторы */

typedef struct {
    int x, y, w, h;
} mon_t;

static mon_t mons[16];
static int nmons;

static int input_font_px(void);

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
    max_mon_h = 0;
    max_mon_w = 0;
    for (int i = 0; i < nmons; i++) {
        if (mons[i].h > max_mon_h)
            max_mon_h = mons[i].h;
        if (mons[i].w > max_mon_w)
            max_mon_w = mons[i].w;
    }
    if (max_mon_w < 800)
        max_mon_w = 800;
    panel_h = sp(46);
    if (getenv("XSNIP_DEBUG")) {
        for (int i = 0; i < nmons; i++)
            fprintf(stderr, "monitor %d: %dx%d+%d+%d\n", i, mons[i].w, mons[i].h,
                    mons[i].x, mons[i].y);
        fprintf(stderr, "поле ввода: %d px (высота %d / 25)\n", input_font_px(), max_mon_h);
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
    if (x >= src->w) x = src->w - 1;
    if (y >= src->h) y = src->h - 1;
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

/* XGetPixel в цикле — это десятки миллионов вызовов на 4K, поэтому для
 * обычного TrueColor 32bpp читаем память напрямую */
static img_t *ximage_to_img(XImage *xi, int w, int h) {
    img_t *im = img_new(w, h);
    int fast = (xi->bits_per_pixel == 32 && xi->byte_order == LSBFirst &&
                visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 &&
                visual->blue_mask == 0xff);
    if (fast) {
        for (int y = 0; y < h; y++) {
            uint32_t *src = (uint32_t *)(xi->data + (size_t)y * xi->bytes_per_line);
            uint32_t *dst = im->px + (size_t)y * w;
            for (int x = 0; x < w; x++)
                dst[x] = src[x] & 0xffffffu;
        }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                im->px[(size_t)y * w + x] = pixel_from_ximage(xi, x, y);
    }
    XDestroyImage(xi);
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

/* mkdir -p своими руками: system() с путём из окружения — это инъекция */
static int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* число независимо от локали: в ru_RU десятичный разделитель — запятая,
 * и atof("2.5") возвращает 2 */
static double parse_double(const char *s, double def) {
    if (!s)
        return def;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *p = buf; *p; p++)
        if (*p == ',')
            *p = '.';
    locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    char *end = NULL;
    double v;
    if (c_loc) {
        v = strtod_l(buf, &end, c_loc);
        freelocale(c_loc);
    } else {
        v = strtod(buf, &end);
    }
    if (end == buf)
        return def;
    return v;
}

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
            c->pen = parse_double(val, c->pen);
        else if (strcmp(key, "text") == 0)
            c->txt = parse_double(val, c->txt);
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
        if (mkdir_p(dir) != 0) { /* каталог не создался — молча продолжаем */ }
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

static int png_encode(const img_t *im, unsigned char **out, size_t *outlen, int level) {
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
    if (compress2(zdata, &zlen, raw, rawlen, level) != Z_OK) {
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
    if (png_encode(im, &data, &len, 6) != 0) {
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

static Atom A_CLIPBOARD, A_TARGETS, A_INCR, A_PNG_IMAGE, A_PNG_UPPER;

#define INCR_MAX 4

typedef struct {
    Window req;
    Atom prop;
    size_t off;
    int active;
    long deadline;      /* если клиент пропал — освобождаем слот */
    long saved_mask;    /* what the requestor had selected before us */
    int mask_saved;
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

static long now_ms(void);

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

static FILE *clip_log;

static void clip_logf(const char *fmt, ...) {
    if (!clip_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(clip_log, "демон %d: ", (int)getpid());
    vfprintf(clip_log, fmt, ap);
    fputc('\n', clip_log);
    va_end(ap);
    fflush(clip_log);
}

static int ack_fd = -1;

static void clipboard_daemon(unsigned char *data, size_t len) {
    /* вызывается уже в потомке */
    Window win = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    if (!win)
        _exit(1);
    XSelectInput(dpy, win, PropertyChangeMask);
    XSetSelectionOwner(dpy, A_CLIPBOARD, win, CurrentTime);
    if (XGetSelectionOwner(dpy, A_CLIPBOARD) != win) {
        clip_logf("не смог стать владельцем selection");
        _exit(1);
    }
    XFlush(dpy);
    clip_logf("владею CLIPBOARD, данных %zu байт", len);
    if (ack_fd >= 0) {
        ssize_t wr = write(ack_fd, "1", 1);
        (void)wr;
        close(ack_fd);
        ack_fd = -1;
    }

    incr_t incs[INCR_MAX];
    memset(incs, 0, sizeof(incs));
    for (;;) {
        XEvent ev;
        /* ждём с таймаутом: брошенный клиент не должен держать слот вечно */
        if (!XPending(dpy)) {
            int fd = XConnectionNumber(dpy);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = {1, 0};
            if (select(fd + 1, &fds, NULL, NULL, &tv) == 0) {
                long t = now_ms();
                for (int i = 0; i < INCR_MAX; i++)
                    if (incs[i].active && t > incs[i].deadline) {
                        if (incs[i].mask_saved)
                            XSelectInput(dpy, incs[i].req, incs[i].saved_mask);
                        incs[i].active = 0;
                        incs[i].mask_saved = 0;
                    }
                continue;
            }
        }
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case SelectionRequest: {
            XSelectionRequestEvent *rq = &ev.xselectionrequest;
            Atom prop = rq->property ? rq->property : rq->target;
            {
                char *tn = XGetAtomName(dpy, rq->target);
                clip_logf("запрос target=%s (0x%lx) от окна 0x%lx",
                          tn ? tn : "?", (unsigned long)rq->target,
                          (unsigned long)rq->requestor);
                if (tn)
                    XFree(tn);
            }
            if (rq->target == A_TARGETS) {
                Atom list[3] = {A_TARGETS, A_PNG_IMAGE, A_PNG_UPPER};
                XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                                (unsigned char *)list, 3);
                notify(rq->requestor, prop, rq->target, rq->time);
            } else if (rq->target == A_PNG_IMAGE || rq->target == A_PNG_UPPER) {
                if (len <= max_prop_bytes()) {
                    XChangeProperty(dpy, rq->requestor, prop, A_PNG_IMAGE, 8,
                                    PropModeReplace, data, (int)len);
                    notify(rq->requestor, prop, rq->target, rq->time);
                } else {
                    /* INCR: сначала объявляем длину, потом льём кусками */
                    int slot = -1;
                    for (int i = 0; i < INCR_MAX; i++)
                        if (!incs[i].active) {
                            slot = i;
                            break;
                        }
                    if (slot < 0) {
                        /* все слоты заняты: честно отказываем */
                        notify(rq->requestor, None, rq->target, rq->time);
                        break;
                    }
                    long n = (long)len;
                    XWindowAttributes wa;
                    if (XGetWindowAttributes(dpy, rq->requestor, &wa)) {
                        incs[slot].saved_mask = wa.your_event_mask;
                        incs[slot].mask_saved = 1;
                        XSelectInput(dpy, rq->requestor,
                                     wa.your_event_mask | PropertyChangeMask);
                    }
                    XChangeProperty(dpy, rq->requestor, prop, A_INCR, 32,
                                    PropModeReplace, (unsigned char *)&n, 1);
                    notify(rq->requestor, prop, rq->target, rq->time);
                    incs[slot].req = rq->requestor;
                    incs[slot].prop = prop;
                    incs[slot].off = 0;
                    incs[slot].active = 1;
                    incs[slot].deadline = now_ms() + 30000;
                }
            } else {
                clip_logf("target не поддерживаю — отказываю");
                notify(rq->requestor, None, rq->target, rq->time);
            }
            break;
        }
        case PropertyNotify: {
            incr_t *slot = NULL;
            for (int i = 0; i < INCR_MAX; i++)
                if (incs[i].active && ev.xproperty.window == incs[i].req &&
                    ev.xproperty.atom == incs[i].prop &&
                    ev.xproperty.state == PropertyDelete) {
                    slot = &incs[i];
                    break;
                }
            if (!slot)
                break;
            size_t chunk = max_prop_bytes();
            size_t left = len - slot->off;
            size_t n = left < chunk ? left : chunk;
            XChangeProperty(dpy, slot->req, slot->prop, A_PNG_IMAGE, 8, PropModeReplace,
                            data + slot->off, (int)n);
            slot->off += n;
            if (n == 0) {
                if (slot->mask_saved)
                    XSelectInput(dpy, slot->req, slot->saved_mask);
                slot->active = 0;
                slot->mask_saved = 0;
            }
            break;
        }
        case SelectionClear:
            clip_logf("SelectionClear — выбор забрали, выхожу");
            _exit(0);
        default:
            break;
        }
    }
}

/* Копирует PNG в буфер: потомок живёт, пока данные не заберут. */
static pid_t clipboard_put_png(const unsigned char *data, size_t len) {
    unsigned char *copy = malloc(len);
    if (!copy)
        die("нет памяти под буфер");
    memcpy(copy, data, len);

    int sync_pipe[2] = {-1, -1};
    if (pipe(sync_pipe) != 0)
        sync_pipe[0] = sync_pipe[1] = -1;

    pid_t pid = fork();
    if (pid < 0) {
        free(copy);
        fprintf(stderr, "xsnip: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (sync_pipe[0] >= 0)
            close(sync_pipe[0]);
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
        ack_fd = sync_pipe[1];
        if (getenv("XSNIP_DEBUG"))
            clip_log = fopen("/tmp/xsnip-clip.log", "a");
        if (clip_log) {
            struct stat st;
            if (fstat(fileno(clip_log), &st) == 0 && st.st_size > 200000) {
                fclose(clip_log);
                clip_log = fopen("/tmp/xsnip-clip.log", "w");
            }
        }
        clipboard_daemon(copy, len);
        _exit(0);
    }
    /* родитель: ждём подтверждения, что демон уже владеет CLIPBOARD —
     * иначе вставка сразу после закрытия окна попадает в пустоту */
    if (sync_pipe[1] >= 0) {
        close(sync_pipe[1]);
        struct timeval tv = {0, 200000};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sync_pipe[0], &rfds);
        char ack = 0;
        if (select(sync_pipe[0] + 1, &rfds, NULL, NULL, &tv) > 0)
            (void)read(sync_pipe[0], &ack, 1);
        close(sync_pipe[0]);
    }
    free(copy);
    int status;
    waitpid(pid, &status, WNOHANG);
    return pid;
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
    GC xor_gc;
    XftDraw *draw;
    XftFont *font;
    int done;
    rect_t rect;
    int drawn;              /* XOR-рамка сейчас на экране */
    int last_x, last_y, last_w, last_h;   /* где показана яркая область */
    int pending_full;       /* выделение изменилось, нужен полный кадр */
    int lab_x, lab_y, lab_w, lab_h;   /* где сейчас нарисована подпись размера */
} sel_t;

static int sel_rect_now(sel_t *s, int *rx, int *ry, int *rw, int *rh);
static int str_width(int px, const char *s);
static void draw_str(XftDraw *draw, uint32_t rgb, int px, int x, int y, const char *s);

/* XOR-рамка: рисуется и стирается одной и той же операцией, без PutImage */
static void sel_xor_frame(sel_t *s) {
    int x, y, w, h;
    if (!sel_rect_now(s, &x, &y, &w, &h))
        return;
    XDrawRectangle(dpy, s->win, s->xor_gc, x, y, w, h);
    XDrawRectangle(dpy, s->win, s->xor_gc, x + 1, y + 1, w - 2, h - 2);
    s->drawn = !s->drawn;
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

/* Полный кадр — только когда мышь остановилась: показываем яркую область */
static void sel_full(sel_t *s) {
    int x, y, w, h;
    if (s->drawn)
        sel_xor_frame(s);              /* стереть рамку */
    if (s->last_w > 0)
        XPutImage(dpy, s->win, s->gc, s->dark, s->last_x, s->last_y, s->last_x,
                  s->last_y, s->last_w, s->last_h);
    if (sel_rect_now(s, &x, &y, &w, &h)) {
        XPutImage(dpy, s->win, s->gc, s->bright, x, y, x, y, w, h);
        s->last_x = x;
        s->last_y = y;
        s->last_w = w;
        s->last_h = h;
    } else {
        s->last_w = s->last_h = 0;
    }
    sel_xor_frame(s);                  /* и снова рамка, уже по яркому */
    if (sel_rect_now(s, &x, &y, &w, &h)) {
        char lab[64];
        snprintf(lab, sizeof(lab), "%d x %d", w, h);
        int tw = str_width(13, lab);
        int lx = x + 4, ly = (y > 26 ? y - 22 : y + 6);
        XSetForeground(dpy, s->gc, 0x000000);
        XFillRectangle(dpy, s->win, s->gc, lx, ly, tw + 8, 18);
        draw_str(s->draw, 0xffffff, 13, lx + 4, ly + 2, lab);
        s->lab_x = lx;
        s->lab_y = ly;
        s->lab_w = tw + 8;
        s->lab_h = 18;
    } else {
        s->lab_w = 0;
    }
    XFlush(dpy);
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
    s.xor_gc = XCreateGC(dpy, s.win, 0, NULL);
    XSetFunction(dpy, s.xor_gc, GXxor);
    XSetForeground(dpy, s.xor_gc, 0xffffff);
    XSetLineAttributes(dpy, s.xor_gc, 1, LineSolid, CapButt, JoinMiter);
    Font fixed = XLoadFont(dpy, "fixed");
    XSetFont(dpy, s.xor_gc, fixed);
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
        if (!XPending(dpy)) {
            int fd = XConnectionNumber(dpy);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 120000;   /* 120 мс тишины — можно показать яркую область */
            if (select(fd + 1, &fds, NULL, NULL, &tv) == 0) {
                if (s.pending_full) {
                    sel_full(&s);
                    s.pending_full = 0;
                }
                continue;
            }
        }
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
            XEvent nx;
            while (XPending(dpy)) {
                XPeekEvent(dpy, &nx);
                if (nx.type != MotionNotify || nx.xmotion.window != s.win)
                    break;
                XNextEvent(dpy, &ev);
            }
            if (s.lab_w > 0) {
                XPutImage(dpy, s.win, s.gc, s.dark, s.lab_x, s.lab_y, s.lab_x,
                          s.lab_y, s.lab_w, s.lab_h);
                s.lab_w = 0;
            }
            if (s.drawn)
                sel_xor_frame(&s);         /* стереть старую рамку */
            s.x1 = ev.xmotion.x;
            s.y1 = ev.xmotion.y;
            sel_xor_frame(&s);             /* нарисовать новую */
            s.pending_full = 1;
            XFlush(dpy);
            break;
        }
        case ButtonRelease: {
            int x, y, w, h;
            if (s.drawn)
                sel_xor_frame(&s);
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
    XFreeGC(dpy, s.xor_gc);
    XFreeGC(dpy, s.gc);
    XUnloadFont(dpy, fixed);
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
    int fam;
    XftFont *font;
} fontent_t;

static fontent_t fonts[96];
static int nfonts;

/* порядок подбора: основной для языка, потом универсальные с латиницей и CJK */
static const char *FAMILIES[4] = {"sans", "DejaVu Sans", "Droid Sans Fallback", "Noto Sans CJK SC"};
#define NFAM 4

static XftFont *font_for(int px, int fam);

static XftFont *font_for(int px, int fam) {
    if (px < 4)
        px = 4;
    if (fam < 0 || fam >= NFAM)
        fam = 0;
    for (int i = 0; i < nfonts; i++)
        if (fonts[i].px == px && fonts[i].fam == fam)
            return fonts[i].font;
    char spec[192];
    snprintf(spec, sizeof(spec), "%s:pixelsize=%d", FAMILIES[fam], px);
    XftFont *f = XftFontOpenName(dpy, scr, spec);
    if (!f) {
        if (fam != 0)
            return font_for(px, 0);
        f = XftFontOpenName(dpy, scr, "sans:pixelsize=13");
        if (!f)
            die("не нашёл шрифт (fontconfig)");
    }
    if (nfonts >= (int)(sizeof(fonts) / sizeof(fonts[0]))) {
        /* кэш кончился: закрываем всё, иначе шрифты текут при ресайзе */
        for (int i = 0; i < nfonts; i++)
            if (fonts[i].font)
                XftFontClose(dpy, fonts[i].font);
        nfonts = 0;
    }
    fonts[nfonts].px = px;
    fonts[nfonts].fam = fam;
    fonts[nfonts].font = f;
    nfonts++;
    return f;
}

static XftFont *font_get(int px) {
    return font_for(px, 0);
}

/* шрифт, в котором реально есть этот символ */
static XftFont *font_cp(int px, unsigned int cp) {
    XftFont *f = font_for(px, 0);
    if (f && f->charset && FcCharSetHasChar(f->charset, cp))
        return f;
    for (int i = 1; i < NFAM; i++) {
        XftFont *g = font_for(px, i);
        if (g && g->charset && FcCharSetHasChar(g->charset, cp))
            return g;
    }
    return f;
}

static int utf8_next(const char *s, unsigned int *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x1f) << 6) | (u[1] & 0x3f);
        return 2;
    }
    if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x0f) << 12) | ((unsigned)(u[1] & 0x3f) << 6) |
              (u[2] & 0x3f);
        return 3;
    }
    if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80 &&
        (u[3] & 0xc0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x07) << 18) | ((unsigned)(u[1] & 0x3f) << 12) |
              ((unsigned)(u[2] & 0x3f) << 6) | (u[3] & 0x3f);
        return 4;
    }
    *cp = 0xfffd;
    return 1;
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

/* Xft не делает fallback между шрифтами, поэтому рисуем по символу: латиница
 * из DejaVu, CJK из подходящего шрифта. */
static void draw_str(XftDraw *draw, uint32_t rgb, int px, int x, int y, const char *s) {
    XftColor col;
    xft_color(rgb, &col);
    int cx = x;
    const char *p = s;
    while (*p) {
        unsigned int cp = 0;
        int len = utf8_next(p, &cp);
        XftFont *f = font_cp(px, cp);
        XftDrawStringUtf8(draw, &col, f, cx, y + font_get(px)->ascent,
                          (const FcChar8 *)p, len);
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, len, &ext);
        cx += ext.xOff;
        p += len;
    }
    XftColorFree(dpy, visual, cmap, &col);
}

static int str_width(int px, const char *s) {
    int w = 0;
    const char *p = s;
    while (*p) {
        unsigned int cp = 0;
        int len = utf8_next(p, &cp);
        XftFont *f = font_cp(px, cp);
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, len, &ext);
        w += ext.xOff;
        p += len;
    }
    return w;
}

/* ------------------------------------------------------------------- ops */

enum { OP_STROKE = 0, OP_TEXT = 1, OP_CROP = 2 };

typedef struct {
    int kind;
    uint32_t color;
    double width;   /* пиксели изображения, для штриха */
    int size;       /* кегль в пикселях изображения, для текста */
    int n;          /* число точек */
    int *pts;       /* x0,y0,x1,y1,... в координатах изображения */
    char *text;
    int tx, ty;     /* левый верх текста */
    img_t *prev;    /* OP_CROP: картинка до обрезки */
    int ox, oy;     /* OP_CROP: на сколько сдвинули всё остальное */
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
        img_free(o->items[i].prev);
    }
    free(o->items);
    memset(o, 0, sizeof(*o));
}

/* ---------------------------------------------------------------- редактор */

enum {
    B_UNDO = 1,
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

    int drawing;
    int *cur_pts;
    int cur_n, cur_cap;
    double cur_w;
    uint32_t cur_color;

    int crop_active;   /* тянем рамку вырезания */
    int cx0, cy0, cx1, cy1;   /* рамка в координатах изображения */
    GC crop_gc;        /* XOR: рамка рисуется и стирается без перерисовки */
    int crop_drawn;
    int crop_anchor;   /* первый клик: ждём второй (режим «клик-клик») */
    int ax, ay;

    int resize_pending;    /* размер изменился, кэш ещё не пересобран */
    long resize_deadline;

    int clip_dirty;        /* в буфере устаревшая картинка */
    long clip_deadline;
    pid_t clip_pid;        /* последний демон буфера */
    int crop_px, crop_py, crop_pw, crop_ph;   /* прошлая рамка в координатах окна */

    int text_active;
    char textbuf[TEXT_MAX];
    int text_len;
    int text_x, text_y;   /* координаты вставки в изображении */

    mon_t mon;       /* экран, на котором живёт окно */
    int cursor_zone; /* 0 — панель, 1 — картинка */
    Cursor cur_crop;

    XftDraw *draw;
    GC cgc;
    Window tip_win;
    GC tip_gc;
    XftDraw *tip_draw;
    int tip_btn;       /* кнопка под курсором, для которой ждём подсказку */
    int tip_shown;
    long tip_deadline;
    char tip_text[128];

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
    int oy = to_window ? e->oy + panel_h : 0;
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

/*
 * Клавиша, не зависящая от раскладки: берём keysym нулевой группы (US), то
 * есть по физической клавише. На русской раскладке та же клавиша отдаёт
 * Cyrillic_pe вместо p, и шорткаты перестают работать.
 */
static KeySym base_keysym(XKeyEvent *ev) {
    KeySym ks = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, 0);
    if (ks == NoSymbol)
        return NoSymbol;
    if (ks >= XK_A && ks <= XK_Z)
        ks = ks + (XK_a - XK_A);
    return ks;
}

/* Семейство шрифта под текущий язык: у DejaVu нет CJK-глифов, поэтому для
 * китайского просим у fontconfig шрифт, который умеет 中. */
static void font_family_init(void) {
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"sans");
    FcCharSet *cs = NULL;
    if (lang == 2) {
        cs = FcCharSetCreate();
        FcCharSetAddChar(cs, 0x4e2d);   /* 中 */
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
    } else if (lang == 1) {
        FcPatternAddString(pat, FC_LANG, (const FcChar8 *)"ru");
    }
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    if (m) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
            snprintf(font_family, sizeof(font_family), "%s", (const char *)fam);
        FcPatternDestroy(m);
    }
    FcPatternDestroy(pat);
    if (cs)
        FcCharSetDestroy(cs);
    if (font_family[0])
        FAMILIES[0] = font_family;
    if (getenv("XSNIP_DEBUG"))
        fprintf(stderr, "шрифт: %s (язык %d)\n", FAMILIES[0], lang);
}

/* --- иконки: рисуем примитивами, никаких картинок и зависимостей */

enum { ICON_NONE = 0, ICON_UNDO, ICON_CHECK, ICON_PEN, ICON_TEXT, ICON_CROP };

static void icon_stroke(editor_t *e, uint32_t col, int w) {
    XSetForeground(dpy, e->cgc, col);
    XSetLineAttributes(dpy, e->cgc, w, LineSolid, CapRound, JoinRound);
}

static void draw_icon(editor_t *e, Drawable d, int icon, int cx, int cy, int sz,
                      uint32_t col) {
    int r = sz / 2;
    switch (icon) {
    case ICON_UNDO: {
        /* дуга против часовой: разрыв справа, стрелка на верхнем конце
         * смотрит влево — классический значок отмены */
        icon_stroke(e, col, 2);
        int rr = r - 1;
        XDrawArc(dpy, d, e->cgc, cx - rr, cy - rr, 2 * rr, 2 * rr, 50 * 64, 280 * 64);
        /* cos(50°)=0.643, sin(50°)=0.766 — без libm */
        int px = cx + (int)(rr * 0.643 + 0.5);
        int py = cy - (int)(rr * 0.766 + 0.5);
        XPoint head[3] = {{px - 6, py - 1}, {px + 1, py - 5}, {px + 1, py + 4}};
        XSetForeground(dpy, e->cgc, col);
        XFillPolygon(dpy, d, e->cgc, head, 3, Convex, CoordModeOrigin);
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
    if (e->nbtn >= (int)(sizeof(e->btns) / sizeof(e->btns[0])) - 1) {
        fprintf(stderr, "xsnip: переполнение списка кнопок\n");
        return x + w;
    }
    btn_t *b = &e->btns[e->nbtn++];
    b->id = id;
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    return x + w;
}



static int tool_width(const char *label) {
    int ico = sp(16), pad = sp(6);
    return sp(8) + ico + pad + str_width(sp(13), label) + sp(10);
}

/* сколько места просит панель — меньше этого окно делать нельзя */
static int panel_width(editor_t *e) {
    int w = sp(8);
    w += sp(34) + sp(10);                          /* отменить */
    w += tool_width(t(STR_PEN)) + sp(4);           /* перо */
    w += tool_width(t(STR_TEXT)) + sp(4);          /* текст */
    w += tool_width(t(STR_CROP)) + sp(10);         /* вырезать */
    w += NCOLORS * (sp(26) + sp(4)) + sp(6);       /* цвета */
    w += sp(26) + sp(4) + str_width(sp(13), "Very wide label 00.0% ") + sp(10) + sp(4) +
         sp(26);
    w += sp(10) + sp(34) + sp(8);                  /* готово */
    (void)e;
    return w;
}

static void editor_relayout(editor_t *e) {
    e->nbtn = 0;
    int x = sp(8), y = sp(6), h = panel_h - sp(12);

    x = ui_button(e, B_UNDO, x, y, sp(34), h);
    x += sp(10);
    x = ui_button(e, B_PEN, x, y, tool_width(t(STR_PEN)), h);
    x += sp(4);
    x = ui_button(e, B_TEXT, x, y, tool_width(t(STR_TEXT)), h);
    x += sp(4);
    x = ui_button(e, B_CROP, x, y, tool_width(t(STR_CROP)), h);
    x += sp(10);

    for (int i = 0; i < NCOLORS; i++) {
        x = ui_button(e, B_COLOR0 + i, x, y, sp(26), h);
        x += sp(4);
    }
    x += sp(6);

    x = ui_button(e, B_SHRINK, x, y, sp(26), h);
    int wlab = str_width(sp(13), "Very wide label 00.0% ") + sp(10);
    x += sp(4) + wlab + sp(4);
    x = ui_button(e, B_GROW, x, y, sp(26), h);

    /* правый край: копировать и «готово», ровно от края окна */
    int xdone = e->cw - sp(8) - sp(34);
    ui_button(e, B_DONE, xdone, y, sp(34), h);
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
    int fpx = sp(13);
    int ico = sp(16);
    int pad = sp(6);
    int line_y = b->y + (b->h - font_get(fpx)->ascent - font_get(fpx)->descent) / 2;
    if (label && icon != ICON_NONE) {
        int tw = str_width(fpx, label);
        int total = ico + pad + tw;
        int x0 = b->x + (b->w - total) / 2;
        draw_icon(e, e->win, icon, x0 + ico / 2, cy, ico, col);
        draw_str(e->draw, col, fpx, x0 + ico + pad, line_y, label);
    } else if (label) {
        int tw = str_width(fpx, label);
        draw_str(e->draw, col, fpx, b->x + (b->w - tw) / 2, line_y, label);
    } else {
        draw_icon(e, e->win, icon, b->x + b->w / 2, cy, ico, col);
    }
}

static void editor_draw_swatch(editor_t *e, btn_t *b, uint32_t col, int active) {
    XSetForeground(dpy, e->cgc, active ? 0x202020 : 0xbdbdb8);
    XFillRectangle(dpy, e->win, e->cgc, b->x, b->y, b->w, b->h);
    int pad = active ? sp(3) : sp(2);
    XSetForeground(dpy, e->cgc, col);
    XFillRectangle(dpy, e->win, e->cgc, b->x + pad, b->y + pad, b->w - 2 * pad,
                   b->h - 2 * pad);
}

static void editor_paint(editor_t *e) {
    XSetForeground(dpy, e->cgc, 0xf2f2ee);
    XFillRectangle(dpy, e->win, e->cgc, 0, 0, e->cw, panel_h);
    XSetForeground(dpy, e->cgc, 0xc9c9c4);
    XFillRectangle(dpy, e->win, e->cgc, 0, panel_h - 1, e->cw, 1);

    editor_relayout(e);

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
        snprintf(lbl, sizeof(lbl), "%s", t(STR_FRAME));
    else if (e->cfg.tool == 1)
        snprintf(lbl, sizeof(lbl), "%s %.1f%%", t(STR_SIZE), e->cfg.txt);
    else
        snprintf(lbl, sizeof(lbl), "%s %.1f%%", t(STR_WIDTH), e->cfg.pen);
    int tw = str_width(sp(13), lbl);
    draw_str(e->draw, 0x202020, sp(13), lab_x + (lab_w - tw) / 2,
             (panel_h - font_get(sp(13))->ascent - font_get(sp(13))->descent) / 2, lbl);

    for (int i = 0; i < e->nbtn; i++) {
        btn_t *b = &e->btns[i];
        switch (b->id) {
        case B_UNDO:
            editor_draw_button(e, b, ICON_UNDO, NULL, 0, e->ops.n > 0);
            break;
        case B_DONE:
            editor_draw_button(e, b, ICON_CHECK, NULL, 0, 1);
            break;
        case B_PEN:
            editor_draw_button(e, b, ICON_PEN, t(STR_PEN), e->cfg.tool == 0, 1);
            break;
        case B_TEXT:
            editor_draw_button(e, b, ICON_TEXT, t(STR_TEXT), e->cfg.tool == 1, 1);
            break;
        case B_CROP:
            editor_draw_button(e, b, ICON_CROP, t(STR_CROP), e->cfg.tool == 2, 1);
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

/* Рамка вырезания: XOR-рисование, чтобы стирать её без перерисовки картинки
 * (раньше каждого движения мыши стоило мегабайт PutImage). */
static void crop_xor(editor_t *e, int x, int y, int w, int h, int label) {
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    XDrawRectangle(dpy, e->win, e->crop_gc, x, y, w, h);
    XDrawRectangle(dpy, e->win, e->crop_gc, x + 1, y + 1, w - 2, h - 2);
    if (label) {
        char lab[64];
        snprintf(lab, sizeof(lab), "%d x %d", w, h);
        int fh = sp(13);
        XDrawString(dpy, e->win, e->crop_gc, x + sp(4),
                    (y > panel_h ? y - sp(4) : y + h + fh + sp(2)), lab, (int)strlen(lab));
    }
}

static void crop_erase(editor_t *e) {
    if (!e->crop_drawn)
        return;
    crop_xor(e, e->crop_px, e->crop_py, e->crop_pw, e->crop_ph, 1);
    e->crop_drawn = 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* что показать при наведении на кнопку; NULL — ничего */
static const char *tip_for(editor_t *e, int id, char *buf, size_t n) {
    switch (id) {
    case B_UNDO:
        return t(STR_UNDO);
    case B_DONE:
        return t(STR_DONE);
    case B_PEN:
        snprintf(buf, n, "%s (p)", t(STR_PEN));
        return buf;
    case B_TEXT:
        snprintf(buf, n, "%s (t)", t(STR_TEXT));
        return buf;
    case B_CROP:
        snprintf(buf, n, "%s (r)", t(STR_CROP));
        return buf;
    case B_SHRINK:
        return t(STR_SHRINK);
    case B_GROW:
        return t(STR_GROW);
    default:
        if (id >= B_COLOR0 && id < B_COLOR0 + NCOLORS) {
            snprintf(buf, n, "%s %d", t(STR_COLOR), id - B_COLOR0 + 1);
            return buf;
        }
        return NULL;
    }
    (void)e;
}

static void tip_hide(editor_t *e) {
    if (e->tip_shown && e->tip_win) {
        XUnmapWindow(dpy, e->tip_win);
        e->tip_shown = 0;
    }
}

static void tip_paint(editor_t *e) {
    if (!e->tip_win)
        return;
    int px = sp(12);
    int w = str_width(px, e->tip_text) + sp(16);
    int h = font_get(px)->ascent + font_get(px)->descent + sp(10);
    XSetForeground(dpy, e->tip_gc, 0xffffe1);
    XFillRectangle(dpy, e->tip_win, e->tip_gc, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, e->tip_gc, 0x606060);
    XDrawRectangle(dpy, e->tip_win, e->tip_gc, 0, 0, (unsigned)(w - 1), (unsigned)(h - 1));
    draw_str(e->tip_draw, 0x101010, px, sp(8), sp(4), e->tip_text);
}

static void tip_show(editor_t *e) {
    char buf[64];
    const char *txt = tip_for(e, e->tip_btn, buf, sizeof(buf));
    if (!txt)
        return;
    int px = sp(12);
    int w = str_width(px, txt) + sp(16);
    int h = font_get(px)->ascent + font_get(px)->descent + sp(10);
    int x = sp(8), y = panel_h + sp(4);
    for (int i = 0; i < e->nbtn; i++) {
        if (e->btns[i].id == e->tip_btn) {
            x = e->btns[i].x;
            if (x + w > e->cw - 4)
                x = e->cw - w - 4;
            break;
        }
    }
    if (!e->tip_win) {
        XSetWindowAttributes a;
        a.override_redirect = True;
        a.background_pixel = 0xffffe1;
        e->tip_win = XCreateWindow(dpy, root, x, y, w, h, 1, CopyFromParent, InputOutput,
                                   CopyFromParent,
                                   CWOverrideRedirect | CWBackPixel, &a);
        e->tip_gc = XCreateGC(dpy, e->tip_win, 0, NULL);
        e->tip_draw = XftDrawCreate(dpy, e->tip_win, visual, cmap);
        XStoreName(dpy, e->tip_win, "xsnip-tip");
    }
    Window child;
    int wx = 0, wy = 0;
    XTranslateCoordinates(dpy, e->win, root, x, y, &wx, &wy, &child);
    XMoveResizeWindow(dpy, e->tip_win, wx, wy, (unsigned)w, (unsigned)h);
    snprintf(e->tip_text, sizeof(e->tip_text), "%s", txt);
    XMapRaised(dpy, e->tip_win);
    e->tip_shown = 1;
    tip_paint(e);
    XFlush(dpy);
}

/* высота строки ввода: от самого высокого монитора, а не от картинки —
 * иначе на маленьком экране редактор получается нечитаемо мелким */
static int input_font_px(void) {
    int px = max_mon_h / 25;
    if (px < 10)
        px = 10;
    if (px > 96)
        px = 96;
    return px;
}

static void editor_blit_canvas(editor_t *e) {
    if (e->resize_pending) {
        /* во время ресайза не пересобираем картинку — просто заливаем фон */
        XSetForeground(dpy, e->cgc, 0x9a9a96);
        XFillRectangle(dpy, e->win, e->cgc, 0, panel_h, (unsigned)e->cw, (unsigned)e->ch);
        return;
    }
    if (!e->view_xi)
        return;
    XPutImage(dpy, e->win, e->cgc, e->view_xi, 0, 0, 0, panel_h, e->cw, e->ch);
    editor_draw_ops(e, e->win, e->draw, 1);
    if (e->text_active) {
        char line[TEXT_MAX + 32];
        snprintf(line, sizeof(line), "%s%s_", t(STR_INPUT), e->textbuf);
        int px = input_font_px();
        int w = str_width(px, line) + sp(20);
        int h = font_get(px)->ascent + font_get(px)->descent + sp(12);
        int x = sp(12), y = panel_h + e->ch - h - sp(12);
        XSetForeground(dpy, e->cgc, 0x202020);
        XFillRectangle(dpy, e->win, e->cgc, x, y, w, h);
        XSetForeground(dpy, e->cgc, 0xffffff);
        XFillRectangle(dpy, e->win, e->cgc, x + 1, y + 1, w - 2, h - 2);
        XSetForeground(dpy, e->cgc, 0x202020);
        XDrawRectangle(dpy, e->win, e->cgc, x, y, w - 1, h - 1);
        draw_str(e->draw, 0x101010, px, x + sp(10), y + sp(6), line);
        /* где именно начнётся текст: вертикальная черта высотой с кегль */
        int cx = e->ox + (int)(e->text_x * e->scale);
        int cy = panel_h + e->oy + (int)(e->text_y * e->scale);
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
    int zone = (y < panel_h) ? 0 : 1;
    if (zone == e->cursor_zone)
        return;
    e->cursor_zone = zone;
    Cursor c;
    if (zone == 0)
        c = e->cur_arrow;
    else
        c = e->cfg.tool == 1 ? e->cur_text
                             : (e->cfg.tool == 2 ? e->cur_crop : e->cur_pen);
    XDefineCursor(dpy, e->win, c);
    XFlush(dpy);
}

static void editor_set_tool(editor_t *e, int tool) {
    if (e->cfg.tool == tool)
        return;
    if (e->crop_anchor) {
        crop_erase(e);
        e->crop_anchor = 0;
    }
    e->cfg.tool = tool;
    if (e->cur_arrow == 0)
        return;
    Cursor c = e->cursor_zone == 0
                   ? e->cur_arrow
                   : (tool == 1 ? e->cur_text : (tool == 2 ? e->cur_crop : e->cur_pen));
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
    img_t *out = ximage_to_img(out_xi, e->iw, e->ih);
    XftDrawDestroy(xd);
    XFreeGC(dpy, g);
    XFreePixmap(dpy, pm);
    return out;
}

/* Буфер всегда содержит то, что сейчас в редакторе: обновляем после каждого
 * изменения, чтобы после выделения области ничего не требовалось нажимать. */
/* отложенное обновление: серия быстрых действий не должна тормозить редактор */
static void editor_clip_mark(editor_t *e) {
    e->clip_dirty = 1;
    e->clip_deadline = now_ms() + 150;
}

/* короткий лог того, что происходит с буфером: пишется всегда */
static void clip_note(const char *fmt, ...) {
    if (!getenv("XSNIP_DEBUG"))
        return;
    FILE *f = fopen("/tmp/xsnip-clip.log", "a");
    if (!f)
        return;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 200000) {
        fclose(f);
        f = fopen("/tmp/xsnip-clip.log", "w");
        if (!f)
            return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "xsnip %d: ", (int)getpid());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

static void editor_clip_update(editor_t *e) {
    e->clip_dirty = 0;
    long t0 = getenv("XSNIP_DEBUG") ? now_ms() : 0;
    img_t *flat = editor_flatten(e);
    unsigned char *png = NULL;
    size_t len = 0;
    if (png_encode(flat, &png, &len, 1) == 0 && png) {
        e->clip_pid = clipboard_put_png(png, len);
        clip_note("буфер обновлён (%zu байт), демон %d", len, (int)e->clip_pid);
        free(png);
    } else {
        clip_note("не смог собрать PNG для буфера");
    }
    img_free(flat);
    if (getenv("XSNIP_DEBUG"))
        fprintf(stderr, "буфер обновлён за %ld мс\n", now_ms() - t0);
}

static void editor_done(editor_t *e) {
    /* «готово» только пишет файл, если его просили */
    if (e->outpath) {
        img_t *flat = editor_flatten(e);
        if (png_write_file(flat, e->outpath) == 0)
            e->saved = 1;
        img_free(flat);
    }
    e->done = 1;
}

static void editor_start_text(editor_t *e, int wx, int wy) {
    if (wy < panel_h)
        return;
    e->text_active = 1;
    e->text_len = 0;
    e->textbuf[0] = 0;
    e->text_x = (int)((wx - e->ox) / e->scale);
    e->text_y = (int)((wy - panel_h - e->oy) / e->scale);
    if (e->text_x < 0) e->text_x = 0;
    if (e->text_y < 0) e->text_y = 0;
    if (e->text_x > e->iw) e->text_x = e->iw;
    if (e->text_y > e->ih) e->text_y = e->ih;
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
    editor_clip_mark(e);
}

/* вырезать прямоугольник из картинки и продолжить редактирование внутри */
static void editor_undo(editor_t *e) {
    if (e->ops.n == 0)
        return;
    op_t *op = &e->ops.items[e->ops.n - 1];
    if (op->kind == OP_CROP) {
        img_free(e->src);
        e->src = op->prev;
        op->prev = NULL;
        e->iw = e->src->w;
        e->ih = e->src->h;
        e->basis = e->iw < e->ih ? e->iw : e->ih;
        for (int i = 0; i < e->ops.n - 1; i++) {
            op_t *o = &e->ops.items[i];
            if (o->kind == OP_STROKE)
                for (int k = 0; k < o->n; k++) {
                    o->pts[k * 2] += op->ox;
                    o->pts[k * 2 + 1] += op->oy;
                }
            else if (o->kind == OP_TEXT) {
                o->tx += op->ox;
                o->ty += op->oy;
            }
        }
        e->ops.n--;
        view_rebuild(e);
    } else if (op->kind == OP_TEXT) {
        free(op->text);
        op->text = NULL;
        e->ops.n--;
    } else {
        free(op->pts);
        op->pts = NULL;
        e->ops.n--;
    }
    editor_repaint(e);
    XFlush(dpy);
    editor_clip_mark(e);
}

static void editor_crop_apply(editor_t *e) {
    crop_erase(e);
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
    op_t *cop = ops_push(&e->ops);
    cop->kind = OP_CROP;
    cop->prev = e->src;     /* старая картинка живёт до отмены */
    cop->ox = x0;
    cop->oy = y0;
    e->src = c;
    e->iw = c->w;
    e->ih = c->h;
    e->basis = e->iw < e->ih ? e->iw : e->ih;

    for (int i = 0; i < e->ops.n - 1; i++) {
        op_t *op = &e->ops.items[i];
        if (op->kind == OP_STROKE)
            for (int k = 0; k < op->n; k++) {
                op->pts[k * 2] -= x0;
                op->pts[k * 2 + 1] -= y0;
            }
        else if (op->kind == OP_TEXT) {
            op->tx -= x0;
            op->ty -= y0;
        }
    }

    view_rebuild(e);
    editor_repaint(e);
    editor_set_tool(e, 0);   /* после обрезки — снова перо */
    XFlush(dpy);
    editor_clip_mark(e);
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
    int winh = e->ih + panel_h < maxh ? e->ih + panel_h : maxh;
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
    e->cur_pen = XCreateFontCursor(dpy, XC_pencil);
    e->cur_crop = XCreateFontCursor(dpy, XC_crosshair);
    e->cur_text = XCreateFontCursor(dpy, XC_xterm);
    e->crop_gc = XCreateGC(dpy, e->win, 0, NULL);
    XSetFunction(dpy, e->crop_gc, GXxor);
    XSetForeground(dpy, e->crop_gc, 0xffffff);
    XSetLineAttributes(dpy, e->crop_gc, 1, LineSolid, CapButt, JoinMiter);
    e->cur_arrow = XCreateFontCursor(dpy, XC_left_ptr);
    e->cursor_zone = 1;
    XDefineCursor(dpy, e->win, e->cfg.tool == 1 ? e->cur_text
                                               : (e->cfg.tool == 2 ? e->cur_crop
                                                                   : e->cur_pen));

    XMapWindow(dpy, e->win);
    XSync(dpy, False);
    editor_focus(e);
    editor_place(e, winw, winh);
    editor_make_ic(e->win);

    e->cw = winw;
    e->ch = winh - panel_h;
    editor_rebuild(e);
    editor_repaint(e);
    XFlush(dpy);
    editor_clip_update(e);   /* выделил — уже в буфере, как у scrot */
    e->clip_dirty = 0;

    for (;;) {
        XEvent ev;
        if (!XPending(dpy)) {
            int fd = XConnectionNumber(dpy);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            long wait = 1000;
            if (e->tip_btn && !e->tip_shown) {
                long left = e->tip_deadline - now_ms();
                wait = left > 0 ? left : 0;
            }
            if (e->resize_pending) {
                long left = e->resize_deadline - now_ms();
                if (left < wait)
                    wait = left > 0 ? left : 0;
            }
            if (e->clip_dirty) {
                long left = e->clip_deadline - now_ms();
                if (left < wait)
                    wait = left > 0 ? left : 0;
            }
            struct timeval tv;
            tv.tv_sec = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            if (select(fd + 1, &fds, NULL, NULL, &tv) == 0) {
                if (e->clip_dirty && now_ms() >= e->clip_deadline)
                    editor_clip_update(e);
                if (e->resize_pending && now_ms() >= e->resize_deadline) {
                    e->resize_pending = 0;
                    editor_rebuild(e);
                    editor_repaint(e);
                    XFlush(dpy);
                }
                if (e->tip_btn && !e->tip_shown && now_ms() >= e->tip_deadline)
                    tip_show(e);
                continue;
            }
        }
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.window == e->tip_win) {
                if (ev.xexpose.count == 0)
                    tip_paint(e);
                break;
            }
            if (ev.xexpose.window == e->win && ev.xexpose.count == 0) {
                editor_repaint(e);
                editor_focus(e);
            }
            break;
        case ConfigureNotify:
            if (ev.xconfigure.window == e->win) {
                int nw = ev.xconfigure.width, nh = ev.xconfigure.height;
                if (nw != e->cw || nh - panel_h != e->ch) {
                    e->cw = nw;
                    e->ch = nh - panel_h;
                    if (e->ch < 2)
                        e->ch = 2;
                    /* пересборка кэша стоит миллионы пикселей — откладываем,
                     * пока пользователь не отпустит рамку */
                    e->resize_pending = 1;
                    e->resize_deadline = now_ms() + 120;
                    editor_paint(e);
                    editor_blit_canvas(e);
                    XFlush(dpy);
                }
            }
            break;
        case MotionNotify: {
            /* пока рисуем рамку или водим мышью без нажатия, важно только
             * последнее положение: лишние события из очереди выбрасываем */
            if (e->crop_active || !e->drawing) {
                XEvent nx;
                while (XPending(dpy)) {
                    XPeekEvent(dpy, &nx);
                    if (nx.type != MotionNotify || nx.xmotion.window != e->win)
                        break;
                    XNextEvent(dpy, &ev);
                }
            }
            int x = ev.xmotion.x, y = ev.xmotion.y;
            if (e->crop_anchor && e->cfg.tool == 2) {
                /* предпросмотр от поставленного угла до курсора */
                crop_erase(e);
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - panel_h - e->oy) / e->scale);
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                if (ix > e->iw) ix = e->iw;
                if (iy > e->ih) iy = e->ih;
                int rx0 = e->ax < ix ? e->ax : ix;
                int ry0 = e->ay < iy ? e->ay : iy;
                int rw = e->ax < ix ? ix - e->ax : e->ax - ix;
                int rh = e->ay < iy ? iy - e->ay : e->ay - iy;
                e->crop_px = e->ox + (int)(rx0 * e->scale);
                e->crop_py = panel_h + e->oy + (int)(ry0 * e->scale);
                e->crop_pw = (int)(rw * e->scale);
                e->crop_ph = (int)(rh * e->scale);
                e->crop_drawn = 1;
                crop_xor(e, e->crop_px, e->crop_py, e->crop_pw, e->crop_ph, 1);
                XFlush(dpy);
                break;
            }
            if (e->crop_active) {
                crop_erase(e);
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - panel_h - e->oy) / e->scale);
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                if (ix > e->iw) ix = e->iw;
                if (iy > e->ih) iy = e->ih;
                e->cx1 = ix;
                e->cy1 = iy;
                int rx0 = e->cx0 < e->cx1 ? e->cx0 : e->cx1;
                int ry0 = e->cy0 < e->cy1 ? e->cy0 : e->cy1;
                int rw = e->cx0 < e->cx1 ? e->cx1 - e->cx0 : e->cx0 - e->cx1;
                int rh = e->cy0 < e->cy1 ? e->cy1 - e->cy0 : e->cy0 - e->cy1;
                e->crop_px = e->ox + (int)(rx0 * e->scale);
                e->crop_py = panel_h + e->oy + (int)(ry0 * e->scale);
                e->crop_pw = (int)(rw * e->scale);
                e->crop_ph = (int)(rh * e->scale);
                e->crop_drawn = 1;
                crop_xor(e, e->crop_px, e->crop_py, e->crop_pw, e->crop_ph, 1);
                XFlush(dpy);
                break;
            }
            if (e->drawing) {
                int ix = (int)((x - e->ox) / e->scale);
                int iy = (int)((y - panel_h - e->oy) / e->scale);
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
                    int y0 = panel_h + e->oy +
                             (int)(e->cur_pts[(e->cur_n - 2) * 2 + 1] * e->scale);
                    int x1 = e->ox + (int)(ix * e->scale);
                    int y1 = panel_h + e->oy + (int)(iy * e->scale);
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
                if (hit != e->tip_btn) {
                    tip_hide(e);
                    e->tip_btn = (y < panel_h) ? hit : 0;
                    e->tip_deadline = now_ms() + 700;
                }
            }
            break;
        }
        case ButtonPress: {
            int x = ev.xbutton.x, y = ev.xbutton.y;
            editor_focus(e);
            tip_hide(e);
            e->tip_btn = 0;
            if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
                editor_adjust(e, ev.xbutton.button == Button4 ? 1 : -1);
                break;
            }
            if (ev.xbutton.button == Button3) {
                if (e->text_active) {
                    e->text_active = 0;
                    editor_blit_canvas(e);
                }
                if (e->crop_anchor) {
                    crop_erase(e);
                    e->crop_anchor = 0;
                }
                break;
            }
            if (ev.xbutton.button != Button1)
                break;
            if (y < panel_h) {
                int id = editor_hit(e, x, y);
                if (id == B_UNDO) {
                    editor_undo(e);
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
                int iy = (int)((y - panel_h - e->oy) / e->scale);
                if (e->crop_anchor) {
                    /* второй клик: рамка от первого угла до этого */
                    e->cx0 = e->ax;
                    e->cy0 = e->ay;
                    e->crop_anchor = 0;
                } else {
                    e->cx0 = ix;
                    e->cy0 = iy;
                }
                e->cx1 = ix;
                e->cy1 = iy;
                e->crop_active = 1;
                e->crop_drawn = 0;
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
                int iy = (int)((y - panel_h - e->oy) / e->scale);
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
                int w = e->cx0 < e->cx1 ? e->cx1 - e->cx0 : e->cx0 - e->cx1;
                int h = e->cy0 < e->cy1 ? e->cy1 - e->cy0 : e->cy0 - e->cy1;
                if (w < 5 || h < 5) {
                    /* это был клик, а не протяжка: запоминаем первый угол */
                    crop_erase(e);
                    e->crop_active = 0;
                    e->crop_anchor = 1;
                    e->ax = e->cx0;
                    e->ay = e->cy0;
                } else {
                    editor_crop_apply(e);
                }
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
                editor_clip_mark(e);
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
                        int k = e->text_len - 1;
                        while (k > 0 && ((unsigned char)e->textbuf[k] & 0xc0) == 0x80)
                            k--;
                        e->text_len = k;
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
            KeySym bk = base_keysym(&ev.xkey);   /* не зависит от раскладки */
            if (bk == NoSymbol)
                bk = ks;
            int ctrl = (ev.xkey.state & ControlMask) != 0;
            if (ctrl && (bk == XK_z || ks == XK_z)) {
                editor_undo(e);
                break;
            }
            if (ks == XK_Escape) {
                /* Esc закрывает так же, как ✓: работа доделывается
                 * (с -o файл пишется), чтобы замена scrot работала без
                 * правок в вызывающем скрипте */
                editor_done(e);
                if (e->done)
                    goto finished;
                break;
            }
            if (ks == XK_Return || ks == XK_KP_Enter) {
                /* Enter вне ввода текста — это кнопка «готово» */
                editor_done(e);
                if (e->done)
                    goto finished;
                break;
            }
            if (bk == XK_1 || bk == XK_p) {
                editor_set_tool(e, 0);
            } else if (bk == XK_2 || bk == XK_t) {
                editor_set_tool(e, 1);
            } else if (bk == XK_3 || bk == XK_r) {
                editor_set_tool(e, 2);
            } else if (bk >= XK_4 && bk < XK_4 + NCOLORS) {
                uint32_t c;
                if (parse_color(e->cfg.colors[bk - XK_4], &c)) {
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
                editor_done(e);
                goto finished;
            }
            break;
        default:
            break;
        }
    }
finished:
    /* на выходе буфер обновляем всегда: кнопка ✓, Esc и крестик должны
     * оставлять одинаковый результат, а демон буфера — живым */
    {
        int alive = (e->clip_pid > 0 && kill(e->clip_pid, 0) == 0);
        Window owner = XGetSelectionOwner(dpy, A_CLIPBOARD);
        clip_note("выход: демон %d %s, владелец CLIPBOARD 0x%lx, грязно=%d",
                  (int)e->clip_pid, alive ? "жив" : "мёртв", (unsigned long)owner,
                  e->clip_dirty);
        if (!alive || owner == None) {
            /* держателя нет — поднимаем заново */
            clip_note("держателя нет, обновляю буфер");
            editor_clip_update(e);
        } else if (e->clip_dirty) {
            /* остались неприменённые правки — дописываем */
            clip_note("были несохранённые правки, обновляю буфер");
            editor_clip_update(e);
        }
    }
    cfg_save(&e->cfg);
    XDefineCursor(dpy, e->win, e->cur_arrow);
    if (e->view_xi) {
        XDestroyImage(e->view_xi);
        e->view_xi = NULL;
    }
    img_free(e->view);
    tip_hide(e);
    if (e->tip_win) {
        XftDrawDestroy(e->tip_draw);
        XFreeGC(dpy, e->tip_gc);
        XDestroyWindow(dpy, e->tip_win);
        e->tip_win = 0;
    }
    XftDrawDestroy(e->draw);
    XFreeGC(dpy, e->crop_gc);
    XFreeGC(dpy, e->cgc);
    XFreeCursor(dpy, e->cur_pen);
    XFreeCursor(dpy, e->cur_crop);
    XFreeCursor(dpy, e->cur_text);
    XFreeCursor(dpy, e->cur_arrow);
    XDestroyWindow(dpy, e->win);
    free(e->cur_pts);
}

/* -------------------------------------------------------------------- CLI */

static void usage(FILE *f) {
    fputs(t(STR_USAGE), f);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    lang_init();
    font_family_init();

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
            delay = parse_double(argv[i], 0.0);
        } else if (!strncmp(a, "--delay=", 8)) {
            delay = parse_double(a + 8, 0.0);
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
            cfg.pen = parse_double(argv[i], cfg.pen);
        } else if (!strcmp(a, "--text")) {
            if (++i >= argc)
                die("нужен кегль");
            cfg.txt = parse_double(argv[i], cfg.txt);
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
            printf("xsnip %s (C, Xlib+Xft+zlib)\n", XSNIP_VERSION);
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

    monitors_init();

    A_CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
    A_TARGETS = XInternAtom(dpy, "TARGETS", False);
    A_INCR = XInternAtom(dpy, "INCR", False);
    A_PNG_IMAGE = XInternAtom(dpy, "image/png", False);
    A_PNG_UPPER = XInternAtom(dpy, "PNG", False);

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
