-- one box, and the plugin that reads settings
return {
  format = 1,
  name = "probe",
  version = "0.1.0",
  app = { id = "openecs.test.probe", name = "Probe" },
  depends = { boxes = "0.1", probe = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
