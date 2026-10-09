-- Run it with: OpenECS --preset examples/9_drag_drop/preset.lua
-- Drag a colour from the palette to the sample, or drop files on the sample from a file manager.
---@type ecs.Preset
return {
  format = 1,
  name = "dragdrop",
  version = "0.1.0",
  app = { id = "openecs.example.dragdrop", name = "Drag and drop" },
  depends = { swatches = "0.1" },
  pluginsDir = ".",
  workspaces = {
    { name = "Main",
      windows = {
        { split = "horizontal",
          { panels = { { type = "swatches.palette" } } },
          { panels = { { type = "swatches.sample" } } },
        },
      },
    },
  },
}
