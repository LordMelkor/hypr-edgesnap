# State machines

Two small state machines matter here. Everything else (focus, workspaces,
maximize, the drag itself) is owned by Hyprland or other plugins and is only
read or requested.

## Ownership

| State | Owner | hypr-edgesnap |
|---|---|---|
| Interactive drag (start, motion, release, cancel) | Hyprland drag controller; started by Super+drag, the titlebar plugin or hypr-xcsd | reads it; may end and restart it on the **same** window once (detach) |
| Maximize / fullscreen and their restore geometry | Hyprland fullscreen controller | requests set/unset only |
| Pre-snap geometry | **hypr-edgesnap snap record** | owns |
| Moving a window between workspaces (push) | Omarchy's `o.workspaces`, else Hyprland | `edgesnap.lua` requests it; the plugin only reports the half (`zone()`) |
| Snap preview | **hypr-edgesnap** | owns; render only |

## 1. Snap record (per window)

States: `FREE` (no record), `SNAPPED(zone, restoreBox, snappedBox)`.
`restoreBox` is the window's geometry before its first snap. It is never
overwritten by a later snap, only consumed.

| From | Event | Guard | To | Effect |
|---|---|---|---|---|
| FREE | Win+Left/Right | eligible, not true fullscreen, zone fits | SNAPPED(half) | unmaximize first if maximized; restoreBox = geometry after that |
| SNAPPED | Win+Left/Right | as above | SNAPPED(half) | restoreBox kept |
| SNAPPED(half) | helper `snap(side)`, side == half, window still at snappedBox | neighbouring workspace within the limit | SNAPPED(opposite half) on that workspace | the desktop moves the window (view follows); restoreBox kept |
| SNAPPED(half) | as above | no neighbour (first workspace, or past `push_limit`) | unchanged | |
| any | Win+Left/Right | zone does not fit (min size) | unchanged | re-maximize if it was maximized |
| any | Win+Up | eligible, not true fullscreen | unchanged | native maximize; Hyprland owns its restore |
| SNAPPED(non-max) | Win+Down | eligible | FREE | unmaximize/unfullscreen natively, then restoreBox (moved with its monitor, clamped to the usable area) |
| SNAPPED(Maximize) | Win+Down | window was maximized | FREE | unmaximize, then restoreBox |
| SNAPPED(Maximize) | Win+Down, Win+Left/Right, drag start | window no longer maximized | FREE | stale record dropped; nothing restored |
| FREE | Win+Down | eligible | FREE | unmaximize/unfullscreen natively only |
| FREE | drop in zone | eligible at apply time | SNAPPED(zone) | restoreBox = box at drag start |
| SNAPPED | drop in zone (no detach happened) | eligible | SNAPPED(zone) | restoreBox kept |
| SNAPPED(non-max) | drag passes 6 px | start size == snappedBox size, not fullscreen | FREE | detach: restoreBox size anchored under cursor; drag restarted |
| SNAPPED(non-max) | drag passes 6 px | size differs (resized by hand) | SNAPPED | no detach; record kept for Win+Down |
| any | window destroyed | | removed | |
| any | window tiled / grouped / special workspace | | unchanged | ignored until eligible again |
| any | plugin unload | | all removed | no window is moved |
| any | config reload | | unchanged | |

Push is done by the `edgesnap.lua` helper, not the plugin: it asks the plugin
which half the window is on (`zone()`: `left`/`right` only while the window
keeps its snapped box, so a moved or resized window snaps again instead), then
moves it with the desktop's own workspace move and snaps it with the plugin.
The record is keyed by window, so it survives the move: Win+Down and drag-away
restore on the new workspace.

Eligible: mapped, floating, not grouped, on a regular (non-special) workspace.

Persistence: records live in compositor memory for the plugin's lifetime. They
survive config reloads, not plugin reloads or logout.

Failure: geometry is applied in-process; a client that refuses a size keeps
the layout's requested box. A zone that cannot hold the window's minimum size
is never offered.

## 2. Drag (one at a time)

`IDLE → TRACKING(startBox, startCursor) → [DETACH] → CANDIDATE(zone) ⇄ TRACKING → APPLY → IDLE`

| From | Event | To | Effect |
|---|---|---|---|
| IDLE | motion during a move drag of an eligible, non-tiled-origin window | TRACKING | record startBox, startCursor; drop stale Maximize record |
| TRACKING | motion < 6 px from start (detach undecided) | TRACKING | no preview |
| TRACKING | first motion ≥ 6 px | DETACH or TRACKING | detach once if the record guard holds; motion event cancelled |
| DETACH | (immediate) | TRACKING(restored box) | end drag, set geometry, begin drag on same window |
| TRACKING | cursor in zone that fits | CANDIDATE | preview |
| CANDIDATE | cursor leaves zone | TRACKING | preview fades |
| CANDIDATE | left-button release | APPLY | deferred to the event loop, after the drag owner ends the drag |
| TRACKING | left-button release before 6 px (detach undecided) | IDLE | a snapped window still at its snapped size goes back to its snapped box (Omarchy's drag_threshold is 0, so the jitter moved it) |
| TRACKING | left-button release | IDLE | |
| any | drag target gone / changed / not a move / disabled | IDLE or new TRACKING | preview fades |
| any | dragged window destroyed | IDLE | |
| APPLY | | IDLE | re-check eligibility, apply, record |

Releases are observed, never consumed, so the titlebar plugin and hypr-xcsd
still see the release that ends their drag (hypr-xcsd forwards it to X11
apps). The restarted drag is on the same window and is not an exclusive grab,
which both rely on.

## Deliberate differences from the source module

- Win+Up is native maximize with no record (as the titlebar button), not a
  recorded snap.
- A manual resize keeps the record: Win+Down still restores the original; only
  drag-detach requires the snapped size.
- Becoming tiled keeps the record.
- Unload drops records instead of moving windows back.
- Border resizing, cascade, focus-border and marker-file features are removed.
