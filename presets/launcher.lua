-- what OpenECS starts with when the command line names no preset and no session: a list of presets and saved sessions
return {
  format = 1,
  name = "launcher",
  version = "0.1.0",
  app = { id = "openecs.launcher", name = "OpenECS" },
  listed = false,
  depends = { launcher = "0.1" },
  workspaces = {
    { name = "Launcher", windows = { { panels = { { type = "launcher.list" } } } } },
  },
}
