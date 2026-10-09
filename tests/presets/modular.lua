-- one box, and a Lua plugin of several files
return {
  format = 1,
  name = "modular",
  version = "0.1.0",
  app = { id = "openecs.test.modular", name = "Modular" },
  depends = { boxes = "0.1", modular = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
