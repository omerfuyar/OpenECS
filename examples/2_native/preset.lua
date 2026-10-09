-- Run it with: OpenECS --preset examples/2_native/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "native",
  version = "0.1.0",
  app = { id = "openecs.example.native", name = "Hello in C" },
  depends = { hello_c = "0.1" },
  pluginsDir = ".",
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "hello_c.greeting" } } } } },
  },
}
