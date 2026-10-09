-- one panel that opens popups
return {
  format = 1,
  name = "popper",
  version = "0.1.0",
  app = { id = "openecs.test.popper", name = "Popper" },
  depends = { popper = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One", windows = { { panels = { { type = "popper.panel" } } } } },
  },
}
