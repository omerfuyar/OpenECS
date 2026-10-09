-- Run it with: OpenECS --preset examples/7_events/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "events",
  version = "0.1.0",
  app = { id = "openecs.example.events", name = "Events" },
  depends = { ticker = "0.1", listener = "0.1" },
  pluginsDir = ".",
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "listener.panel" } } } } },
  },
}
