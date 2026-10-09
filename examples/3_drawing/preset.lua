-- Run it with: OpenECS --preset examples/3_drawing/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "drawing",
  version = "0.1.0",
  app = { id = "openecs.example.drawing", name = "Drawing" },
  depends = { button = "0.1" },
  pluginsDir = ".",
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "button.panel" } } } } },
  },
}
