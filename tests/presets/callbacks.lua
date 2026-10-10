return {
  format = 1,
  name = "callbacks",
  version = "0.1.0",
  app = { id = "openecs.test.callbacks", name = "Callbacks" },
  depends = { boxes = "0.1", cboxes = "0.1", callbacks = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Boxes",
      windows = {
        { split = "horizontal",
          { panels = { { type = "boxes.one" } } },
          { panels = { { type = "cboxes.box" } } },
        },
      },
    },
  },
}
