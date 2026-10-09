-- Run it with: OpenECS --preset examples/5_services/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "services",
  version = "0.1.0",
  app = { id = "openecs.example.services", name = "Services" },
  depends = { tally = "0.1" }, -- tally depends on counter, so counter loads too
  pluginsDir = ".",
  workspaces = {
    { name = "Main",
      windows = {
        { split = "horizontal",
          { panels = { { type = "tally.panel" } } },
          { panels = { { type = "tally.panel" } } },
        },
      },
    },
  },
}
