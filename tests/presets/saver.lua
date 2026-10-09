-- a box in each of two workspaces, and the plugin that saves the session
return {
  format = 1,
  name = "saver",
  version = "0.1.0",
  app = { id = "openecs.test.saver", name = "Saver" },
  depends = { boxes = "0.1", saver = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "boxes.one" } } } } },
    { name = "Two", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
