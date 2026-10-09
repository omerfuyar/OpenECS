-- Run it with: OpenECS --preset examples/4_settings/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "settings",
  version = "0.1.0",
  app = { id = "openecs.example.settings", name = "Settings" },
  depends = { greeter = "0.1" },
  pluginsDir = ".",
  settings = { ["greeter.name"] = "OpenECS" }, -- a preset sets values over the plugin's defaults; the user's settings win over both
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "greeter.panel" } } } } },
  },
}
