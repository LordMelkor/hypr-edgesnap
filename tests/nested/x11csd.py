"""X11 client-side-decorated test window for the nested-compositor harness.

Usage: EDGESNAP_NESTED=1 python3 -I x11csd.py TITLE

Draws no titlebar of its own; the top 30 px act as one. Button 1 press there
sends _NET_WM_MOVERESIZE (move) to the root window, as GTK/Qt CSD clients do.
Stdout protocol: "ready", "press X Y", "release X Y". Exits on SIGTERM or
stdin EOF.
"""
import ctypes as C
import os
import select
import signal
import sys

if os.environ.get('EDGESNAP_NESTED') != '1' or not os.environ.get('DISPLAY'):
    sys.stderr.write('x11csd: needs EDGESNAP_NESTED=1 and DISPLAY (nested session only)\n')
    sys.exit(2)
if len(sys.argv) != 2:
    sys.stderr.write('usage: x11csd.py TITLE\n')
    sys.exit(2)
title = sys.argv[1].encode()

lib = C.CDLL('libX11.so.6')
ptr, ul, integer, lng = C.c_void_p, C.c_ulong, C.c_int, C.c_long


def api(name, restype, args):
    f = getattr(lib, name)
    f.restype = restype
    f.argtypes = args
    return f


ButtonPress, ButtonRelease, MapNotify, ClientMessage = 4, 5, 19, 33
ButtonPressMask, ButtonReleaseMask = 1 << 2, 1 << 3
ExposureMask, StructureNotifyMask = 1 << 15, 1 << 17
SubstructureNotifyMask, SubstructureRedirectMask = 1 << 19, 1 << 20
MOVE, TITLEBAR_H = 8, 30
CurrentTime, XA_STRING, XA_CARDINAL = 0, 31, 6


class XButtonEvent(C.Structure):
    _fields_ = [('type', integer), ('serial', ul), ('send_event', integer),
                ('display', ptr), ('window', ul), ('root', ul),
                ('subwindow', ul), ('time', ul), ('x', integer),
                ('y', integer), ('x_root', integer), ('y_root', integer),
                ('state', C.c_uint), ('button', C.c_uint),
                ('same_screen', integer)]


class XClientMessageEvent(C.Structure):
    _fields_ = [('type', integer), ('serial', ul), ('send_event', integer),
                ('display', ptr), ('window', ul), ('message_type', ul),
                ('format', integer), ('data', lng * 5)]


class XEvent(C.Union):
    _fields_ = [('type', integer), ('xbutton', XButtonEvent),
                ('xclient', XClientMessageEvent), ('pad', lng * 24)]


class XClassHint(C.Structure):
    _fields_ = [('res_name', C.c_char_p), ('res_class', C.c_char_p)]


open_display = api('XOpenDisplay', ptr, [C.c_char_p])
default_root = api('XDefaultRootWindow', ul, [ptr])
create_window = api('XCreateSimpleWindow', ul, [ptr, ul, integer, integer, C.c_uint, C.c_uint, C.c_uint, ul, ul])
intern = api('XInternAtom', ul, [ptr, C.c_char_p, integer])
store_name = api('XStoreName', integer, [ptr, ul, C.c_char_p])
set_class = api('XSetClassHint', integer, [ptr, ul, C.POINTER(XClassHint)])
change_prop = api('XChangeProperty', integer, [ptr, ul, ul, ul, integer, integer, ptr, integer])
select_input = api('XSelectInput', integer, [ptr, ul, lng])
map_window = api('XMapWindow', integer, [ptr, ul])
destroy_window = api('XDestroyWindow', integer, [ptr, ul])
close_display = api('XCloseDisplay', integer, [ptr])
flush = api('XFlush', integer, [ptr])
sync = api('XSync', integer, [ptr, integer])
pending = api('XPending', integer, [ptr])
next_event = api('XNextEvent', integer, [ptr, C.POINTER(XEvent)])
conn_fd = api('XConnectionNumber', integer, [ptr])
ungrab_pointer = api('XUngrabPointer', integer, [ptr, ul])
send_event = api('XSendEvent', integer, [ptr, ul, integer, lng, C.POINTER(XEvent)])

d = open_display(None)
if not d:
    sys.stderr.write('x11csd: cannot open display\n')
    sys.exit(1)
root = default_root(d)
win = create_window(d, root, 200, 200, 800, 600, 0, 0, 0xffffff)
atom = lambda n: intern(d, n.encode(), 0)


def set_prop(prop, typ, fmt, buf, n):
    change_prop(d, win, prop, typ, fmt, 0, C.cast(buf, ptr), n)


store_name(d, win, title)
set_prop(atom('_NET_WM_NAME'), atom('UTF8_STRING'), 8, C.create_string_buffer(title), len(title))
hint = XClassHint(b'edgesnap-x11', b'edgesnap-x11')
set_class(d, win, C.byref(hint))
set_prop(atom('_NET_WM_PID'), XA_CARDINAL, 32, (ul * 1)(os.getpid()), 1)
motif = atom('_MOTIF_WM_HINTS')
set_prop(motif, motif, 32, (ul * 5)(2, 0, 0, 0, 0), 5)  # flags=decorations, decorations=0
select_input(d, win, ButtonPressMask | ButtonReleaseMask | StructureNotifyMask | ExposureMask)
map_window(d, win)
flush(d)


def out(line):
    sys.stdout.write(line + '\n')
    sys.stdout.flush()


def start_move(x_root, y_root):
    ungrab_pointer(d, CurrentTime)
    e = XEvent()
    c = e.xclient
    c.type = ClientMessage
    c.display = d
    c.window = win
    c.message_type = atom('_NET_WM_MOVERESIZE')
    c.format = 32
    c.data[:] = [x_root, y_root, MOVE, 1, 1]
    send_event(d, root, 0, SubstructureRedirectMask | SubstructureNotifyMask, C.byref(e))
    flush(d)


def on_term(*_):
    raise SystemExit(0)


signal.signal(signal.SIGTERM, on_term)
fd, stdin_open, ready = conn_fd(d), True, False
ev = XEvent()
try:
    while stdin_open:
        while pending(d):
            next_event(d, C.byref(ev))
            t = ev.type
            if t == MapNotify and not ready:
                ready = True
                out('ready')
            elif t == ButtonPress:
                b = ev.xbutton
                out('press %d %d' % (b.x, b.y))
                if b.button == 1 and b.y < TITLEBAR_H:
                    start_move(b.x_root, b.y_root)
            elif t == ButtonRelease:
                b = ev.xbutton
                out('release %d %d' % (b.x, b.y))
        try:
            r, _, _ = select.select([fd, 0], [], [])
        except InterruptedError:
            continue
        if 0 in r:
            try:
                if not os.read(0, 4096):
                    stdin_open = False
            except OSError:
                stdin_open = False
finally:
    destroy_window(d, win)
    sync(d, 0)
    close_display(d)
