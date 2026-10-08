-- a canvas and a clock, with the keys after the prefix changed: K closes the panel, and X does nothing
return {
  format = 1,
  name = "changed-keys",
  version = "0.1.0",
  app = { id = "openecs.test.changed_keys", name = "Changed keys" },
  depends = { sketch_c = "0.1" },
  settings = { ["ecs.prefix_keys"] = { K = "ecs.close", X = false } },
  workspaces = {
    { name = "Canvas",
      windows = {
        { split = "horizontal",
          { panels = { { type = "sketch_c.canvas" } } },
          { size = 260, panels = { { type = "sketch_c.clock" } } },
        },
      },
    },
  },
}
