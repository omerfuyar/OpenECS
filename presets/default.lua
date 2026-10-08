return {
  format = 1,
  name = "default",
  version = "0.1.0",
  app = { id = "openecs.default", name = "OpenECS" },
  depends = { demo = "0.1" },
  settings = { ["demo.blink_seconds"] = 0.75 },
  workspaces = {
    { name = "main",
      windows = {
        { split = "horizontal",
          { size = 220, panels = { { type = "demo.color", state = { color = 2 } } } },
          { share = 1, split = "vertical",
            { share = 2, panels = { { type = "demo.gradient" }, { type = "demo.checker" } } },
            { share = 1, panels = { { type = "demo.blink" }, { type = "demo.color" }, { type = "demo.missing" } } },
          },
        },
      },
    },
    { name = "second",
      windows = { { panels = { { type = "demo.checker" } } } },
    },
  },
}
