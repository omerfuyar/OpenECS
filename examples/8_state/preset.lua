-- Run it with: OpenECS --preset examples/8_state/preset.lua
-- Quit and run it again: the marks come back, because the tool's last session keeps them.
---@type ecs.Preset
return {
  format = 1,
  name = "state",
  version = "0.1.0",
  app = { id = "openecs.example.state", name = "State" },
  depends = { marks = "0.1" },
  pluginsDir = ".",
  workspaces = {
    { name = "Main",
      windows = {
        { split = "horizontal",
          { panels = { { type = "marks.panel" } } },
          -- a preset may give a panel a saved state to start from
          { panels = { { type = "marks.panel", stateVersion = 1, state = { dots = { 40, 40, 80, 80 } } } } },
        },
      },
    },
  },
}
