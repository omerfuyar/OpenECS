-- one board that draws with the ui plugin
return {
  format = 1,
  name = "painter",
  version = "0.1.0",
  app = { id = "openecs.test.painter", name = "Painter" },
  depends = { painter = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Board", windows = { { panels = { { type = "painter.board" } } } } },
  },
}
