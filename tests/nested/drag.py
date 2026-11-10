#!/usr/bin/env python3
"""Behavior tests for hypr-edgesnap in a disposable nested Hyprland.

    make hypr-edgesnap.so tests/nested/vptr && python3 tests/nested/drag.py

Needs a running Hyprland session (the nested compositor's GPU comes from it)
and foot. The nested compositor first appears as a window of the session; a
temporary named window rule sends that window, unfocused and floating at the
test size, to a hidden special workspace. The rule is disabled again on exit. Input goes only to the nested compositor
(virtual pointer); the session's cursor, focus and windows are not touched,
and the script checks that afterwards.

Optional plugins loaded into the nested compositor when present:
EDGESNAP_TITLEBARS (Omarchy's titlebar plugin) and EDGESNAP_XCSD (hypr-xcsd).
EDGESNAP_PLUGIN overrides the plugin under test. EDGESNAP_HYPRLAND_ROOT runs
another Hyprland as the nested compositor: an extracted package root with
usr/bin/Hyprland, usr/bin/hyprctl and its libraries in usr/lib (plugins must be
built against that version).
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PLUGIN = Path(os.environ.get('EDGESNAP_PLUGIN') or ROOT / 'hypr-edgesnap.so')
VPTR = HERE / 'vptr'
RULE = 'edgesnap-nested-test'
TITLEBARS = os.environ.get('EDGESNAP_TITLEBARS', '/usr/lib/omarchy-hyprland-titlebars/titlebars.so')
_ALT = os.environ.get('EDGESNAP_HYPRLAND_ROOT')
NESTED_HYPRLAND = str(Path(_ALT, 'usr/bin/Hyprland')) if _ALT else 'Hyprland'
NESTED_HYPRCTL = str(Path(_ALT, 'usr/bin/hyprctl')) if _ALT else 'hyprctl'
NESTED_LIBS = {'LD_LIBRARY_PATH': str(Path(_ALT, 'usr/lib'))} if _ALT else {}
BAR, BORDER = 30, 2
TOP = BAR + BORDER  # titlebar drawn above the border (bar_precedence_over_border)
W, H = 2560, 1080

results = []


def check(cond, label, detail=''):
    if not cond:
        raise AssertionError(f'{label}: {detail}')
    results.append(label)


# ---- host session (read, plus one temporary named rule) -------------------

def host_env():
    runtime = Path('/run/user') / str(os.getuid())
    sockets = list((runtime / 'hypr').glob('*/.socket.sock'))
    if len(sockets) != 1:
        sys.exit('need exactly one running Hyprland session')
    displays = sorted(p.name for p in runtime.glob('wayland-*') if not p.name.endswith('.lock'))
    if not displays:
        sys.exit('no Wayland socket for the session')
    return {**os.environ, 'XDG_RUNTIME_DIR': str(runtime), 'HYPRLAND_INSTANCE_SIGNATURE': sockets[0].parent.name}, \
        str(runtime / displays[-1])


def host_ctl(env, *args):
    return subprocess.run(['hyprctl', *args], env=env, capture_output=True, text=True, timeout=5).stdout


def host_snapshot(env):
    active = json.loads(host_ctl(env, '-j', 'activewindow') or '{}').get('address')
    cursor = host_ctl(env, 'cursorpos').strip()
    windows = sorted((w['address'], tuple(w['at']), tuple(w['size']), w['workspace']['id'])
                     for w in json.loads(host_ctl(env, '-j', 'clients')) if w.get('class') != 'aquamarine')
    return active, cursor, windows


def host_rule(env, enabled):
    code = ('hl.window_rule({ name = "%s", match = { class = "^aquamarine$" }, '
            'workspace = "special:edgesnap-test silent", no_initial_focus = true, float = true, size = "%d %d" })' % (RULE, W, H)) if enabled else \
        'hl.window_rule({ name = "%s", enabled = false })' % RULE
    out = host_ctl(env, 'eval', code).strip()
    if out != 'ok':
        raise RuntimeError(f'host rule: {out}')


# ---- nested compositor ----------------------------------------------------

class Nested:
    def __init__(self, host, display):
        # Short path: Wayland socket paths are limited to 108 bytes.
        self.runtime = Path(tempfile.mkdtemp(prefix='esn-', dir='/tmp'))
        self.home = self.runtime / 'home'
        self.home.mkdir()
        env = {'HOME': str(self.home), 'PATH': os.environ['PATH'], 'XDG_RUNTIME_DIR': str(self.runtime),
               'WAYLAND_DISPLAY': display,
               # No seat for the nested compositor: its DRM backend must fail
               # instead of touching the session's GPU.
               'LIBSEAT_BACKEND': 'seatd', 'SEATD_SOCK': str(self.runtime / 'no-seatd'), 'AQ_DRM_DEVICES': '/nonexistent',
               'EDGESNAP_TITLEBARS': TITLEBARS if Path(TITLEBARS).exists() else '',
               **({'HYPR_XCSD_DEBUG': '1'} if os.environ.get('HYPR_XCSD_DEBUG') else {})}
        self.windows = []
        self.proc = None
        self.x11_display = None
        self.log = open(self.runtime / 'stdout.log', 'w')
        try:
            self.start(env)
        except BaseException:
            self.close()
            raise

    def start(self, env):
        self.proc = subprocess.Popen([NESTED_HYPRLAND, '-c', str(HERE / 'hyprland.lua')], env={**env, **NESTED_LIBS}, stdout=self.log, stderr=subprocess.STDOUT,
                                     start_new_session=True, preexec_fn=lambda: __import__('resource').setrlimit(
                                         __import__('resource').RLIMIT_CORE, (0, 0)))
        for _ in range(100):
            sockets = list((self.runtime / 'hypr').glob('*/.socket.sock'))
            if sockets and self.proc.poll() is None:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError('nested Hyprland did not start; see ' + str(self.runtime / 'stdout.log'))
        self.env = {'PATH': os.environ['PATH'], 'XDG_RUNTIME_DIR': str(self.runtime), 'HOME': str(self.home),
                    'HYPRLAND_INSTANCE_SIGNATURE': sockets[0].parent.name}
        for _ in range(50):
            if self.ctl('-j', 'monitors').startswith('['):
                break
            time.sleep(0.1)
        # The nested output is its window on the session (hidden, see
        # host_rule). Headless outputs can't allocate buffers on every GPU.
        for _ in range(100):
            monitors = json.loads(self.ctl('-j', 'monitors') or '[]')
            if len(monitors) == 1 and (monitors[0]['width'], monitors[0]['height']) == (W, H):
                break
            time.sleep(0.1)
        check([(m['width'], m['height'], m['x'], m['y']) for m in monitors] == [(W, H, 0, 0)],
              'nested compositor has one 2560x1080 output', [(m['name'], m['width'], m['height']) for m in monitors])
        self.wayland = sorted(p.name for p in self.runtime.glob('wayland-*') if not p.name.endswith('.lock'))[0]

    def ctl(self, *args):
        return subprocess.run([NESTED_HYPRCTL, *args], env={**self.env, **NESTED_LIBS}, capture_output=True, text=True, timeout=5).stdout

    def eval(self, code):
        out = self.ctl('eval', code).strip()
        if out != 'ok':
            raise RuntimeError(f'eval {code!r}: {out}')

    def value(self, expr):
        out = self.ctl('eval', 'error("RESULT=" .. tostring(%s), 0)' % expr)
        return out.split('RESULT=', 1)[1].strip() if 'RESULT=' in out else out

    def plugin(self, action, path):
        out = self.ctl('plugin', action, str(path)).strip()
        if out != 'ok':
            raise RuntimeError(f'plugin {action}: {out}')

    def open_window(self, title):
        env = {**self.env, 'WAYLAND_DISPLAY': self.wayland}
        self.windows.append(subprocess.Popen(['foot', '-T', title, 'sleep', 'infinity'], env=env,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for _ in range(100):
            if self.client(title):
                time.sleep(0.2)
                return self.client(title)
            time.sleep(0.05)
        raise RuntimeError('test window did not map')

    def client(self, title):
        return next((c for c in json.loads(self.ctl('-j', 'clients')) if c['title'] == title and c['mapped']), None)

    def focus(self, title):
        self.eval('hl.dispatch(hl.dsp.focus({ window = "title:^%s$" }))' % title)

    def close(self):
        for w in self.windows:
            w.kill()
        if self.proc and self.proc.poll() is None:
            # SIGKILL, not SIGTERM: a nested Hyprland 0.56.2 that has run an
            # X11 client segfaults while shutting down (also with no plugins
            # loaded), and the crash would be reported on the desktop.
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(5)
        if self.x11_display:  # Xwayland was killed with it; drop its socket and lock
            n = self.x11_display.lstrip(':').split('.')[0]
            for stale in (Path(f'/tmp/.X11-unix/X{n}'), Path(f'/tmp/.X11-unix/X{n}_'), Path(f'/tmp/.X{n}-lock')):
                stale.unlink(missing_ok=True)
        self.log.close()
        keep = os.environ.get('EDGESNAP_KEEP_LOG')
        if keep:
            for log in self.runtime.glob('hypr/*/hyprland.log'):
                shutil.copy(log, keep)
            if (self.runtime / 'hypr-xcsd.log').exists():
                shutil.copy(self.runtime / 'hypr-xcsd.log', keep + '.xcsd')
        shutil.rmtree(self.runtime, ignore_errors=True)


class Pointer:
    def __init__(self, nested):
        env = {**nested.env, 'WAYLAND_DISPLAY': nested.wayland, 'EDGESNAP_NESTED': '1'}
        self.proc = subprocess.Popen([str(VPTR), str(W), str(H)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def send(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        if reply != 'ok':
            raise RuntimeError(f'vptr {line!r}: {reply!r}')

    def path(self, *points, steps=8):
        """Hold the button along straight segments between points."""
        x, y = points[0]
        self.send(f'move {x} {y}')
        self.send('wait 30')
        self.send('down')
        for nx, ny in points[1:]:
            for i in range(1, steps + 1):
                self.send(f'move {round(x + (nx - x) * i / steps)} {round(y + (ny - y) * i / steps)}')
                self.send('wait 8')
            x, y = nx, ny
        self.send('wait 30')
        self.send('up')
        self.send('wait 120')

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(3)
        except Exception:
            self.proc.kill()


# ---- scenarios ------------------------------------------------------------

def box(c):
    return (c['at'][0], c['at'][1], c['size'][0], c['size'][1])


LEFT_HALF = (10 + BORDER, 10 + TOP, 1265 - 2 * BORDER, 1060 - TOP - BORDER)
RIGHT_HALF = (1285 + BORDER, 10 + TOP, 1265 - 2 * BORDER, 1060 - TOP - BORDER)
TOP_LEFT = (10 + BORDER, 10 + TOP, 1265 - 2 * BORDER, 525 - TOP - BORDER)
CENTER_THIRD = (10 + 840 + 5 + 5 + BORDER, 10 + TOP, 840 - 2 * BORDER, 1060 - TOP - BORDER)  # (2540-20)/3 = 840


def titlebar_point(c, fraction=0.25):
    x, y, w, _ = box(c)
    return round(x + w * fraction), y - BORDER - BAR // 2


def run(nested, ptr, titlebars):
    t = 'edgesnap-a'
    original = box(nested.open_window(t))
    nested.focus(t)
    snap = lambda action: nested.value(f'hl.plugin.edgesnap.{action}()')
    now = lambda: box(nested.client(t))
    fullscreen = lambda: nested.client(t)['fullscreen']

    # Keyboard.
    check(snap('left') == 'true' and now() == LEFT_HALF, 'Win+Left fills the left half inside gaps and below the titlebar', now())
    check(snap('right') == 'true' and now() == RIGHT_HALF, 'Win+Right moves it to the right half', now())
    check(snap('restore') == 'true' and now() == original, 'Win+Down after two snaps restores the geometry from before the first', now())
    check(snap('restore') == 'true' and now() == original, 'Win+Down on an unsnapped window changes nothing', now())
    snap('left'); snap('maximize')
    check(fullscreen() == 1, 'Win+Up maximizes natively', fullscreen())
    check(snap('maximize') == 'true' and fullscreen() == 1, 'repeated Win+Up stays maximized')
    snap('restore')
    check(fullscreen() == 0 and now() == original, 'Win+Down from maximize after a half snap restores the original', now())
    snap('left')
    nested.eval('hl.dispatch(hl.dsp.window.resize({ x = 1000, y = 700, relative = false }))')
    resized = now()
    check(resized[2:] == (1000, 700), 'setup: snapped window resized by hand', resized)
    check(snap('restore') == 'true' and now() == original, 'Win+Down after a manual resize still restores the original', now())

    if not titlebars:
        print('titlebar plugin not available: drag scenarios skipped', file=sys.stderr)
        return

    # Drag to an edge, then away.
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 300, sy + 100), (2, 540))
    check(now() == LEFT_HALF, 'dropping at the left edge snaps to the left half', now())
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 3, sy))
    check(now() == LEFT_HALF, 'a click with a few pixels of jitter keeps the snap', now())
    ptr.path((sx, sy), (sx + 40, sy + 40), (1200, 300))
    after = now()
    check(after[2:] == original[2:], 'dragging a snapped window away restores its size', after)
    check(after[0] - BORDER <= 1200 <= after[0] + after[2] + BORDER and after[1] - TOP <= 300 <= after[1],
          'the cursor stays on the titlebar of the restored window', after)
    check(snap('restore') == 'true' and now() == after, 'the detached window has no record left', now())

    # A window resized after snapping keeps its size when dragged.
    pre_snap = now()
    snap('left')
    nested.eval('hl.dispatch(hl.dsp.window.resize({ x = 1000, y = 700, relative = false }))')
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 200, sy + 150))
    check(now()[2:] == (1000, 700), 'a snapped window resized by hand keeps that size when dragged', now())
    snap('restore')
    check(now() == pre_snap, 'and Win+Down still returns it to where it was before the snap', (now(), pre_snap))

    # Top edge maximizes; Win+Down returns to where the drag started.
    pre_drag = now()
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (1280, 300), (1280, 1))
    check(fullscreen() == 1, 'dropping at the top edge maximizes', fullscreen())
    snap('restore')
    check(fullscreen() == 0 and now() == pre_drag, 'Win+Down from a drag maximize returns to the pre-drag box', now())

    # Native maximize, dragged away: Hyprland's restore, no plugin record.
    snap('maximize')
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 40, sy + 60), (1000, 400))
    check(fullscreen() == 0 and now()[2:] == original[2:], 'a natively maximized window dragged away gets its size back', now())

    # Half snap, then maximize, then dragged away: back to the original size.
    snap('left'); snap('maximize')
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 40, sy + 60), (1000, 400))
    check(fullscreen() == 0 and now()[2:] == original[2:], 'half snap + maximize + drag away restores the pre-snap size', now())

    # Corner → quarter; thirds when enabled.
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (300, 200), (3, 3))
    check(now() == TOP_LEFT, 'the top-left corner snaps to a quarter', now())
    nested.eval('hl.config({ plugin = { edgesnap = { columns = "3" } } })')
    sx, sy = titlebar_point(nested.client(t))
    ptr.path((sx, sy), (sx + 40, sy + 40), (1280, 900), (1280, 1078))
    check(now() == CENTER_THIRD, 'with columns = 3 the bottom center snaps to the middle third', now())
    nested.eval('hl.config({ plugin = { edgesnap = { columns = "2" } } })')

    # The drag is over after each release: a press in the client area moves nothing.
    cx, cy, cw, ch = now()
    ptr.path((cx + cw // 2, cy + ch // 2), (cx + cw // 2 + 120, cy + ch // 2 + 80))
    check(now() == (cx, cy, cw, ch), 'after consecutive drags a plain press-and-move in the app does not drag the window', now())
    if nested.value('hl.plugin.xcsd ~= nil') == 'true':
        check(nested.value('hl.plugin.xcsd.drag_active()') == 'false', 'no native drag is left active')

    # Unload stops snapping without moving windows; loading again works.
    snap('restore'); snap('left')
    before = now()
    nested.plugin('unload', PLUGIN)
    check(now() == before, 'unloading leaves windows where they are', now())
    check(nested.value('hl.plugin.edgesnap') == 'nil', 'unloading removes hl.plugin.edgesnap')
    nested.plugin('load', PLUGIN)
    check(snap('restore') == 'true' and now() == before, 'after reloading there is no stale record', now())
    check(snap('right') == 'true' and now() == RIGHT_HALF, 'the reloaded plugin snaps', now())


X11_LEFT = (10 + BORDER, 10 + BORDER, 1265 - 2 * BORDER, 1060 - 2 * BORDER)  # no server titlebar


def run_x11(nested, ptr):
    """An X11 app that draws its own titlebar and asks the WM to move it
    (_NET_WM_MOVERESIZE), through hypr-xcsd."""
    if nested.value('hl.plugin.xcsd ~= nil') != 'true':
        print('hypr-xcsd not loaded: X11 scenario skipped', file=sys.stderr)
        return
    display = nested.value('os.getenv("DISPLAY")')
    check(display.startswith(':'), 'nested Xwayland is running', display)
    nested.x11_display = display
    env = {'PATH': os.environ['PATH'], 'HOME': str(nested.home), 'DISPLAY': display, 'EDGESNAP_NESTED': '1'}
    app = subprocess.Popen([sys.executable, '-I', str(HERE / 'x11csd.py'), 'edgesnap-x11'], env=env,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    events = []
    threading.Thread(target=lambda: [events.append(l.split()[0]) for l in app.stdout], daemon=True).start()
    try:
        for _ in range(100):
            c = nested.client('edgesnap-x11')
            if c and 'ready' in events:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('X11 test window did not map')
        time.sleep(0.3)
        c = nested.client('edgesnap-x11')
        check(any(t.rstrip('*') == 'x11-csd' for t in c['tags']), 'hypr-xcsd tags the X11 window as client-decorated', c['tags'])
        original = box(c)
        now = lambda: box(nested.client('edgesnap-x11'))

        def drag(*points):
            # Small steps, like a real mouse: hypr-xcsd forwards the release
            # only while the pointer is still on the app's surface, and X11
            # surfaces follow a fast jump a little late (seen without this
            # plugin loaded too).
            events.clear()
            ptr.path(*points, steps=40)
            time.sleep(0.2)
            return list(events)

        x, y, _, _ = original
        seen = drag((x + 200, y + 10), (x + 300, y + 60))
        moved = now()
        check(moved[:2] != (x, y) and moved[2:] == original[2:] and seen == ['press', 'release'],
              'control: a plain drag by its own titlebar moves it and the app sees press and release', (moved, seen))
        x, y, _, _ = moved
        seen = drag((x + 200, y + 10), (x + 300, y + 60), (2, 540))
        check(now() == X11_LEFT, "dragging an X11 app by its own titlebar to the left edge snaps it", now())
        check(seen[:1] == ['press'] and seen[-1:] == ['release'], 'the app sees the press and the release that ends the drag', seen)

        seen = drag((X11_LEFT[0] + 400, X11_LEFT[1] + 10), (X11_LEFT[0] + 440, X11_LEFT[1] + 50), (1200, 300))
        after = now()
        check(after[2:] == original[2:], 'dragging the snapped X11 app away restores its size', after)
        check(seen[:1] == ['press'] and seen[-1:] == ['release'], 'the app still gets the release after the detach restarted the drag', seen)

        cx, cy, cw, ch = after
        seen = drag((cx + cw // 2, cy + ch // 2), (cx + cw // 2 + 60, cy + ch // 2 + 40))
        check(now() == after and seen == ['press', 'release'], 'a later click in the X11 app reaches it and moves nothing', (now(), seen))
    finally:
        app.stdin.close()
        try:
            app.wait(3)
        except subprocess.TimeoutExpired:
            app.kill()


def main():
    for f in (PLUGIN, VPTR):
        if not f.exists():
            sys.exit(f'build {f.relative_to(ROOT)} first')
    host, display = host_env()
    before = host_snapshot(host)
    host_rule(host, True)
    nested = ptr = None
    try:
        nested = Nested(host, display)
        xcsd = os.environ.get('EDGESNAP_XCSD', '')
        if xcsd and Path(xcsd).exists():
            nested.plugin('load', xcsd)
        nested.plugin('load', PLUGIN)
        titlebars = nested.value('hl.plugin.hyprbars ~= nil') == 'true'
        ptr = Pointer(nested)
        run(nested, ptr, titlebars)
        if titlebars:
            run_x11(nested, ptr)
    finally:
        if ptr:
            ptr.close()
        if nested:
            nested.close()
        host_rule(host, False)
        after = host_snapshot(host)
        print(json.dumps({'passed': results, 'host_unchanged': before == after}, indent=1))
        if before != after:
            print('host session changed:', before, after, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
