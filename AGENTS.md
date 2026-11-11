# Intent and change guardrails

## User intent

Coherent window snapping for floating windows on Omarchy: drag-to-edge zones
with a preview, keyboard halves/maximize/restore, and the pre-snap size back
when a snapped window is dragged away. Keyboard and mouse share one record per
window. Fit in beside Omarchy's native workspaces, titlebars and maximize and
hypr-xcsd, never replace them. Keep maintenance conventional through hyprpm.

## Invariants

- Snapping only. No workspace management, titlebars, borders, resizing,
  cascading, background services or repair loops. The one exception is the
  push in `edgesnap.lua`: the config helper asks the desktop's own
  move-to-workspace (Omarchy's or Hyprland's) to move the window; the plugin
  never moves windows between workspaces.
- Hyprland owns the interactive drag, maximize/fullscreen and their restore
  geometry. The plugin may end and restart a drag once, on the same window,
  non-exclusively (detach); it never consumes a button event.
- One record per window, defined in STATE.md. Change STATE.md with any
  behaviour change, and keep the nested test in step with it.
- Unloading never moves windows.
- hyprpm owns building and loading. No competing loader or builder.
- Failures leave a usable desktop: a missing plugin means no drag snapping and
  keyboard bindings falling back to whatever the user configured.
- A successful build or IPC reply is not behavioural proof. The nested test
  is; live desktop checks need the session unlocked and must restore focus,
  cursor and window geometry.
- Never write a username, home path, email or private file name into this
  repository; commits use the GitHub no-reply address.

## Discipline

Tie each change to a concrete bug, unmet invariant, or demonstrated
maintenance cost. Prefer the smallest change. Name tension between
requirements instead of silently weakening one. Don't push as a side effect.
