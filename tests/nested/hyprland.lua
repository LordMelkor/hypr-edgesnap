-- Config for the disposable nested compositor used by tests/nested/drag.py.
-- Mirrors Omarchy's floating-workspace setup closely enough for snapping:
-- gaps, borders, the Omarchy titlebar plugin and drag_threshold 0.
hl.config({
  general = { gaps_in = 5, gaps_out = 10, border_size = 2, resize_on_border = true, extend_border_grab_area = 8 },
  binds = { drag_threshold = 0 },
  misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
  animations = { enabled = false },
  xwayland = { enabled = true },
  debug = { disable_logs = false },
})
hl.monitor({ output = "", mode = "2560x1080", position = "0x0", scale = 1 })
hl.window_rule({ name = "float-all", match = { class = ".*" }, float = true, size = "800 600", center = true })

local titlebars = os.getenv("EDGESNAP_TITLEBARS")
if titlebars and titlebars ~= "" then
  hl.plugin.load(titlebars)
  if hl.plugin.hyprbars then
    hl.config({ plugin = { hyprbars = {
      enabled = true, bar_height = 30, bar_part_of_window = true, bar_precedence_over_border = true,
    } } })
    -- As in hypr-xcsd's Omarchy fragment: X11 apps with their own titlebar get no second one.
    hl.window_rule({ name = "hypr-xcsd-no-titlebar", match = { tag = "x11-csd" }, ["hyprbars:no_bar"] = true })
  end
end
