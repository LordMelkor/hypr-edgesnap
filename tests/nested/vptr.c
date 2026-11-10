/* Virtual-pointer driver for the nested-compositor test harness.
 * Refuses to run unless explicitly pointed at a private nested session. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define BTN_LEFT 0x110

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void global(void *d, struct wl_registry *r, uint32_t name,
                   const char *iface, uint32_t ver) {
    (void)d;
    if (!seat && !strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 1 ? ver : 1);
    else if (!mgr && !strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface,
                               ver < 1 ? ver : 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_l = { global, global_remove };

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
    const char *nested = getenv("EDGESNAP_NESTED");
    const char *wd = getenv("WAYLAND_DISPLAY");
    const char *xrd = getenv("XDG_RUNTIME_DIR");
    /* The nested compositor lives in a private runtime dir and is reached by
       socket name only; an absolute WAYLAND_DISPLAY could name the desktop. */
    if (!nested || strcmp(nested, "1") || !wd || !*wd || strchr(wd, '/') ||
        !xrd || !*xrd || !strncmp(xrd, "/run/user/", 10)) {
        fprintf(stderr, "vptr: refusing to run outside a nested test session\n");
        return 2;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: vptr WIDTH HEIGHT\n");
        return 1;
    }
    uint32_t W = (uint32_t)strtoul(argv[1], NULL, 10);
    uint32_t H = (uint32_t)strtoul(argv[2], NULL, 10);
    if (!W || !H) {
        fprintf(stderr, "vptr: bad extent\n");
        return 1;
    }

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "vptr: cannot connect to compositor\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!seat) { fprintf(stderr, "vptr: no wl_seat\n"); return 1; }
    if (!mgr) {
        fprintf(stderr, "vptr: compositor lacks zwlr_virtual_pointer_manager_v1\n");
        return 1;
    }
    struct zwlr_virtual_pointer_v1 *ptr =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(dpy);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        unsigned x, y, ms;
        if (sscanf(line, "move %u %u", &x, &y) == 2) {
            zwlr_virtual_pointer_v1_motion_absolute(ptr, now_ms(), x, y, W, H);
            zwlr_virtual_pointer_v1_frame(ptr);
        } else if (!strcmp(line, "down") || !strcmp(line, "up")) {
            zwlr_virtual_pointer_v1_button(ptr, now_ms(), BTN_LEFT,
                line[0] == 'd' ? WL_POINTER_BUTTON_STATE_PRESSED
                               : WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(ptr);
        } else if (sscanf(line, "wait %u", &ms) == 1) {
            struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        } else if (!strcmp(line, "quit")) {
            break;
        } else {
            printf("err %s\n", line);
            fflush(stdout);
            continue;
        }
        wl_display_roundtrip(dpy);
        printf("ok\n");
        fflush(stdout);
    }
    free(line);
    zwlr_virtual_pointer_v1_destroy(ptr);
    wl_display_roundtrip(dpy);
    return 0;
}
