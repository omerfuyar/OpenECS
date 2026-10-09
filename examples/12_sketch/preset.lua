-- The sketch example's preset: three workspaces with the canvases and clocks of sketch_c and sketch_lua.
-- Its plugins are in this folder, so pluginsDir names it.
---@type ecs.Preset
return {
  format = 1,
  name = "sketch",
  version = "0.1.0",
  app = { id = "openecs.sketch", name = "Sketch" },
  depends = { sketch_c = "0.1", sketch_lua = "0.1" },
  pluginsDir = ".",
  settings = { ["sketch_c.brushColor"] = "red", ["sketch_lua.brushColor"] = "green" },
  open = "sketch_c.open",
  keys = { ["Ctrl+Tab"] = "sketch_c.nextWorkspace" },
  workspaces = {
    { name = "C",
      windows = {
        { split = "horizontal",
          { share = 3, panels = { { type = "sketch_c.canvas" } } },
          { size = 260, panels = { { type = "sketch_c.clock" } } },
        },
      },
    },
    { name = "Lua",
      keys = { ["Ctrl+Tab"] = "sketch_lua.nextWorkspace" },
      windows = {
        { split = "horizontal",
          { share = 3, panels = { { type = "sketch_lua.canvas" } } },
          { size = 260, panels = { { type = "sketch_lua.clock" } } },
        },
      },
    },
    { name = "Side by side",
      windows = {
        { split = "vertical",
          { share = 3, split = "horizontal",
            { panels = { { type = "sketch_c.canvas" } } },
            { panels = { { type = "sketch_lua.canvas" } } },
          },
          { share = 1, panels = { { type = "sketch_c.clock" }, { type = "sketch_lua.clock" } } },
        },
      },
    },
  },
}
