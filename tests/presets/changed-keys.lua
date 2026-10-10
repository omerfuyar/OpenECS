-- two boxes, with the keys after the prefix changed: K closes the panel, and X does nothing
return {
  format = 1,
  name = "changed-keys",
  version = "0.1.0",
  app = { id = "openecs.test.changedKeys", name = "Changed keys" },
  pluginsDir = "../plugins",
  depends = { boxes = "0.1" },
  keys = { prefix = { K = "ecs.layout.close", X = false } },
  workspaces = {
    { name = "Canvas",
      windows = {
        { split = "horizontal",
          { panels = { { type = "boxes.one" } } },
          { size = 260, panels = { { type = "boxes.two" } } },
        },
      },
    },
  },
}
