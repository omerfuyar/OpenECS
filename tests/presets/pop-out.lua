-- a box beside a group of two, and a pop-out window with a C box; a second workspace with one box
return {
  format = 1,
  name = "pop-out",
  version = "0.1.0",
  app = { id = "openecs.test.popOut", name = "Pop-out" },
  depends = { boxes = "0.1", cboxes = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "One",
      windows = {
        { split = "horizontal",
          { panels = { { type = "boxes.one" } } },
          { panels = { { type = "boxes.two" }, { type = "boxes.three" } } },
        },
        { width = 300, height = 200, panels = { { type = "cboxes.box" } } },
      },
    },
    { name = "Two", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
