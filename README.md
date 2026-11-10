# hypr-edgesnap

Window snapping for floating windows on Hyprland, made for Omarchy's floating workspaces:

- **Drag to an edge** to snap: left/right halves, corner quarters, top to maximize. Optional thirds. A translucent preview shows where the window will go.
- **Drag a snapped window away** and it gets its previous size back, under the cursor, and the drag carries on.
- **Keyboard** halves, maximize and restore share the same memory as the mouse: restore returns a window to its size and place from before its first snap, however it was snapped.

It only snaps. Hyprland keeps owning the drag, maximize and fullscreen, so dragging by Super + drag, Omarchy's titlebar, or an X11 app's own titlebar (with [hypr-xcsd](https://github.com/LordMelkor/hypr-xcsd)) all work the same. It doesn't manage workspaces, titlebars, borders or resizing.

The zone detection, zone geometry and preview animation come from the MIT-licensed snap module of [omarchy-windows](https://github.com/jwm3000/omarchy-windows) (`contrib/aero-snap`), cut down to snapping and reworked so keyboard and mouse share one record per window. See `LICENSE`.

Tested on Hyprland 0.56.2 (daily use) and 0.57.2 (the nested behaviour suite, with Omarchy's 0.57 titlebars and hypr-xcsd); `compat.hpp` covers the API differences. The keyboard functions need a Lua config (`hyprland.lua`).

## Install

With [hyprpm](https://wiki.hypr.land/Plugins/Using-Plugins/):

```sh
hyprpm update
hyprpm add https://github.com/LordMelkor/hypr-edgesnap
hyprpm enable hypr-edgesnap
hyprpm reload
```

For a private copy of this repository, hyprpm's `git clone` needs your Git credentials (for example `gh auth setup-git`).

hyprpm builds the plugin against your exact Hyprland and rebuilds it on `hyprpm update`. To load it at login, your config must run `hyprpm reload` at startup (hypr-xcsd's `xcsd.lua` does; otherwise see the top of `edgesnap.lua`).

### Omarchy

Copy `edgesnap.lua` to `~/.config/hypr/edgesnap.lua` and load it after Omarchy's defaults in `~/.config/hypr/hyprland.lua`:

```lua
do
  local ok, err = pcall(require, "hypr.edgesnap")
  if not ok then print("edgesnap failed to load: " .. tostring(err)) end
end
```

It sets the options below and runs the optional health check at login. Install that with `install -m 755 scripts/hypr-edgesnap-check ~/.local/bin/`: it reports (and with `--notify`, pops up) when the plugin is enabled but didn't load.

To rebuild after Hyprland updates, use an Omarchy post-update hook that runs `hyprpm update` (hypr-xcsd ships one, `hooks/50-hyprpm`, which covers every hyprpm plugin).

### Keyboard

The plugin adds four Lua functions. Each acts on the focused window and returns whether it handled it:

| Function | Does |
|---|---|
| `hl.plugin.edgesnap.left()` / `right()` | snap to the left/right half (always halves) |
| `hl.plugin.edgesnap.maximize()` | native maximize, the same as the titlebar button |
| `hl.plugin.edgesnap.restore()` | leave maximize/fullscreen; if snapped, go back to the pre-snap size and place |

`edgesnap.lua` has a `bind` helper with example bindings. Omarchy uses Super + arrows for focus, so pick free keys. The helper can fall back to a shell command when the plugin isn't loaded.

## Options

Set with `hl.config({ plugin = { edgesnap = { ... } } })`.

| Option | Default | |
|---|---|---|
| `enabled` | `true` | |
| `columns` | `"2"` | full-height drag zones: `"2"` halves, `"3"` thirds (left/right edge and bottom edge thirds), `"auto"` thirds on monitors wider than 16:9 |
| `quarters` | `true` | corners snap to quarters; `false` makes them halves |
| `gaps` | `true` | keep `gaps_out` around and `gaps_in` between snapped windows |
| `edge_threshold` | `12` | logical pixels from an edge that count as "at the edge" |
| `corner_ratio` | `0.25` | share of each edge that belongs to a corner |
| `preview_color`, `preview_border_color` | blue | preview fill and border |
| `preview_border_size`, `preview_rounding` | `2`, `8` | |
| `preview_blur` | `true` | |
| `preview_animation_duration` | `200` | milliseconds; `0` turns animation off |

## Behaviour details

The full state machines are in `STATE.md`. In short:

- **Which windows:** mapped, floating, not grouped, on a regular workspace. Tiled windows, groups and special workspaces are left alone.
- **One record per window:** its geometry from before the first snap. Snapping again (keyboard or mouse, any zone) keeps it. Restore, drag-away and closing the window use it up.
- **Drag-away** happens after 6 px of movement, and only while the window still has its snapped size. A window resized by hand after snapping keeps its size when dragged; restore still takes it back to the original.
- **A click on a snapped titlebar** (released within 6 px) leaves it exactly in place.
- **Maximize** is Hyprland's own. Dropping on the top edge maximizes and remembers where the drag started, for restore. Keyboard maximize doesn't add a record, so restore after a half snap and a maximize goes all the way back.
- **Restore** keeps the window on screen: it follows its monitor if that moved, and is pulled inside the usable area (below the bar, titlebar included) if needed.
- **Unloading** stops snapping and forgets all records. No window is moved.

## Develop

```sh
make                 # builds hypr-edgesnap.so against the headers pkg-config finds
make test            # geometry unit tests and health-check tests; no compositor needed
make nested-test     # drag and keyboard behaviour in a throwaway nested Hyprland (see below)
```

`make nested-test` needs a running Hyprland session and `foot`. It starts a separate Hyprland as a window of your session, hidden on a special workspace without focus by a temporary named window rule (disabled again on exit), and drives it with a virtual pointer of its own. Your cursor, focus and windows aren't touched; the script compares them before and after. It loads Omarchy's titlebar plugin when installed, and hypr-xcsd when `EDGESNAP_XCSD` points at its `.so`. To test another Hyprland version, point `EDGESNAP_HYPRLAND_ROOT` at an extracted package (with its libraries in `usr/lib`) and `EDGESNAP_PLUGIN`, `EDGESNAP_TITLEBARS` and `EDGESNAP_XCSD` at plugins built for it.

CI builds and runs `make test` against Hyprland 0.56.2 and Arch's latest. It never loads the plugin.

Testing local changes through hyprpm: `hyprpm add "file://$PWD"`. hyprpm builds committed source only. Contributor guardrails: `AGENTS.md`.

## Remove

```sh
hyprpm disable hypr-edgesnap
hyprpm remove hypr-edgesnap
```

Then delete the `edgesnap.lua` load block and file, and any key bindings that call `hl.plugin.edgesnap`.
