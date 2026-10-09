-- a preset, for the tests that list presets
return {
  format = 1,
  name = "paint",
  version = "0.1.0",
  app = { id = "org.example.Paint", name = "Paint" },
  workspaces = { { name = "Canvas", windows = { { panels = { { type = "canvas.view" } } } } } },
}
