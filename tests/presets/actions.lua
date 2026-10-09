-- two boxes side by side, a second workspace, and the plugin that runs the core's functions
return {
  format = 1,
  name = "actions",
  version = "0.1.0",
  app = { id = "openecs.test.actions", name = "Actions" },
  depends = { boxes = "0.1", actions = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One",
      windows = {
        { split = "horizontal",
          { panels = { { type = "boxes.one" } } },
          { panels = { { type = "boxes.one" } } },
        },
      },
    },
    { name = "Two", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
