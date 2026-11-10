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

-- Keyboard snapping. Each action falls back to `fallback` (a shell command,
-- optional) only when the plugin isn't loaded or fails, so a keyboard helper
-- you used before keeps working if the plugin is ever missing.
--   bind("SUPER + CTRL + LEFT", "left")   -- also "right", "maximize", "restore"
local function bind(keys, action, fallback)
  o.rebind(keys, "Snap window " .. action, function()
    local plugin = hl.plugin.edgesnap
    if plugin and plugin[action] and pcall(plugin[action]) then return end
    if fallback then hl.exec_cmd(fallback) end
  end)
end

-- Example bindings (Omarchy uses SUPER + arrows to move focus):
-- bind("SUPER + CTRL + LEFT", "left")
-- bind("SUPER + CTRL + RIGHT", "right")
-- bind("SUPER + CTRL + UP", "maximize")
-- bind("SUPER + CTRL + DOWN", "restore")

-- Report at login if the plugin didn't load (optional health check, see README).
hl.on("hyprland.start", function()
  hl.exec_cmd('[ -x "$HOME/.local/bin/hypr-edgesnap-check" ] && "$HOME/.local/bin/hypr-edgesnap-check" --wait 30 --notify')
end)

return { bind = bind }
