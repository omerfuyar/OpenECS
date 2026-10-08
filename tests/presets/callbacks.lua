return {
  format = 1,
  name = "callbacks",
  version = "0.1.0",
  app = { id = "openecs.test.callbacks", name = "Callbacks" },
  depends = { sketch_c = "0.1", sketch_lua = "0.1", callbacks = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Canvases",
      windows = {
        { split = "horizontal",
          { panels = { { type = "sketch_c.canvas" } } },
          { panels = { { type = "sketch_lua.canvas" } } },
        },
      },
    },
  },
}
