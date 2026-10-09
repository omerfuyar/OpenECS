-- one box, and the plugin that lists presets and sessions
return {
  format = 1,
  name = "lister",
  version = "0.1.0",
  app = { id = "openecs.test.lister", name = "Lister" },
  depends = { boxes = "0.1", lister = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
