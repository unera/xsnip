/*
 * Тестовая обвязка: подсовывает xsnip два «монитора» поверх одного Xvfb.
 * LD_PRELOAD=fake_xinerama.so  XSNIP_FAKE_HEADS="1920x1080+0+0,1920x1200+0+1080"
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

static int heads(XineramaScreenInfo **out) {
    static XineramaScreenInfo buf[8];
    static int n = -1;
    if (n >= 0) {
        *out = buf;
        return n;
    }
    n = 0;
    const char *spec = getenv("XSNIP_FAKE_HEADS");
    if (!spec)
        spec = "1920x1080+0+0,1920x1200+0+1080";
    char *copy = strdup(spec);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && n < 8;
         tok = strtok_r(NULL, ",", &save)) {
        int w, h, x, y;
        if (sscanf(tok, "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
            buf[n].screen_number = n;
            buf[n].x_org = x;
            buf[n].y_org = y;
            buf[n].width = w;
            buf[n].height = h;
            n++;
        }
    }
    free(copy);
    *out = buf;
    return n;
}

Bool XineramaIsActive(Display *dpy) {
    (void)dpy;
    return True;
}

XineramaScreenInfo *XineramaQueryScreens(Display *dpy, int *number) {
    (void)dpy;
    XineramaScreenInfo *src;
    int n = heads(&src);
    XineramaScreenInfo *res = calloc((size_t)n, sizeof(*res));
    memcpy(res, src, (size_t)n * sizeof(*res));
    *number = n;
    return res;
}
