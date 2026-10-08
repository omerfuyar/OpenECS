-- two canvases side by side, a second workspace, and the plugin that runs the core's functions
return {
  format = 1,
  name = "actions",
  version = "0.1.0",
  app = { id = "openecs.test.actions", name = "Actions" },
  depends = { sketch_c = "0.1", actions = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One",
      windows = {
        { split = "horizontal",
          { panels = { { type = "sketch_c.canvas" } } },
          { panels = { { type = "sketch_c.canvas" } } },
        },
      },
    },
    { name = "Two", windows = { { panels = { { type = "sketch_c.canvas" } } } } },
  },
}
