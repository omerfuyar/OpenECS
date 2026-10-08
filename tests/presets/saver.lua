-- a canvas in each of two workspaces, and the plugin that saves the session
return {
  format = 1,
  name = "saver",
  version = "0.1.0",
  app = { id = "openecs.test.saver", name = "Saver" },
  depends = { sketch_c = "0.1", saver = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "sketch_c.canvas" } } } } },
    { name = "Two", windows = { { panels = { { type = "sketch_c.canvas" } } } } },
  },
}
