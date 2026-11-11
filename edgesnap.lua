-- Install this config fragment in ~/.config/hypr/edgesnap.lua and load it
-- after Omarchy's defaults (see README). hyprpm owns building and loading the
-- plugin; this only configures it, binds keys and checks it loaded.
--
-- It does not run `hyprpm reload` itself. If nothing else in your config does
-- (hypr-xcsd's xcsd.lua does), add:
--   hl.permission({ binary = "/usr/bin/hyprpm", type = "plugin", mode = "allow" })
--   hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload") end)

-- The plugin reloads the config once it is in; that pass finds it here.
if hl.plugin.edgesnap then
  hl.config({
    plugin = {
      edgesnap = {
        columns = "2", -- "3" or "auto" adds thirds when dragging to an edge
        quarters = true,
        gaps = true,
      },
    },
  })
end

-- Pushing past an edge. Win+Right on a window already snapped to the right half
-- moves it to the next workspace on the same monitor (created if needed) and
-- snaps it to the left half there; Win+Left does the reverse. The move is the
-- desktop's own: Omarchy's per-monitor workspaces when present, Hyprland's
-- otherwise. The plugin only reports which half the window is on.
local M = {
  -- Highest workspace a push may reach: a slot of the monitor's own on Omarchy
  -- (whose bar shows five), the workspace number elsewhere. false: no limit.
  push_limit = 5,
}

local function push(step)
  local window = hl.get_active_window()
  local id = window and window.workspace and tonumber(window.workspace.id)
  if not id or id < 1 then return false end

  local omarchy = rawget(_G, "o")
  local workspaces = omarchy and omarchy.workspaces
  local slots = workspaces and workspaces.slots
  local slot = slots and (id - 1) % slots + 1 or id
  local target = slot + step
  if target < 1 or (slots and target > slots) or (M.push_limit and target > M.push_limit) then
    return false
  end

  if slots then
    workspaces.move_window(target)()
  else
    hl.dispatch(hl.dsp.window.move({ workspace = tostring(target), follow = true, window = "address:" .. window.address }))
  end
  hl.dispatch(hl.dsp.focus({ window = "address:" .. window.address }))
  return true
end

-- Snap to a half ("left" or "right"), or push if already snapped there. At the
-- limit the window stays where it is. Returns whether the plugin handled it.
function M.snap(side)
  local plugin = hl.plugin.edgesnap
  if not plugin then return false end
  if plugin.zone and plugin.zone() == side then
    if push(side == "right" and 1 or -1) then
      return plugin[side == "right" and "left" or "right"]()
    end
    return true
  end
  return plugin[side]()
end

-- Keyboard snapping. Each action falls back to `fallback` (a shell command,
-- optional) only when the plugin isn't loaded or fails, so a keyboard helper
-- you used before keeps working if the plugin is ever missing.
--   bind("SUPER + CTRL + LEFT", "left")   -- also "right", "maximize", "restore"
local function bind(keys, action, fallback)
  o.rebind(keys, "Snap window " .. action, function()
    local plugin = hl.plugin.edgesnap
    if plugin and plugin[action] then
      local fn = (action == "left" or action == "right") and M.snap or plugin[action]
      if pcall(fn, action) then return end
    end
    if fallback then hl.exec_cmd(fallback) end
  end)
end
M.bind = bind

-- Example bindings (Omarchy uses SUPER + arrows to move focus):
-- bind("SUPER + CTRL + LEFT", "left")
-- bind("SUPER + CTRL + RIGHT", "right")
-- bind("SUPER + CTRL + UP", "maximize")
-- bind("SUPER + CTRL + DOWN", "restore")

-- Report at login if the plugin didn't load (optional health check, see README).
hl.on("hyprland.start", function()
  hl.exec_cmd('[ -x "$HOME/.local/bin/hypr-edgesnap-check" ] && "$HOME/.local/bin/hypr-edgesnap-check" --wait 30 --notify')
end)

return M
