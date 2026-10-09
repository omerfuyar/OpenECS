-- one canvas, and the plugin that reads settings
return {
  format = 1,
  name = "probe",
  version = "0.1.0",
  app = { id = "openecs.test.probe", name = "Probe" },
  depends = { sketch_c = "0.1", probe = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "sketch_c.canvas" } } } } },
  },
}
