-- a preset that asks not to be listed, for the tests that list presets
return {
  format = 1,
  name = "hidden",
  version = "0.1.0",
  app = { id = "org.example.Hidden", name = "Hidden" },
  listed = false,
  workspaces = { { name = "Empty", windows = { {} } } },
}
