-- one pad that takes text input
return {
  format = 1,
  name = "typist",
  version = "0.1.0",
  app = { id = "openecs.test.typist", name = "Typist" },
  depends = { typist = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Pad", windows = { { panels = { { type = "typist.pad" } } } } },
  },
}
