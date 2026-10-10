-- two boxes side by side, and the plugin that moves panels into pop-out windows
return {
  format = 1,
  name = "windows",
  version = "0.1.0",
  app = { id = "openecs.test.windows", name = "Windows" },
  depends = { boxes = "0.1", windows = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One",
      windows = {
        { split = "horizontal", { panels = { { type = "boxes.one" } } }, { panels = { { type = "boxes.two" } } } },
      },
    },
  },
}
