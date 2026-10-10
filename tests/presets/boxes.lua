-- Boxes in three workspaces: two boxes side by side, then one box, then two boxes over a group of two
return {
  format = 1,
  name = "boxes",
  version = "0.1.0",
  app = { id = "openecs.test.boxes", name = "Boxes" },
  depends = { boxes = "0.1", cboxes = "0.1" },
  pluginsDir = "../plugins",
  open = "boxes.open",
  workspaces = {
    { name = "One",
      windows = {
        { split = "horizontal",
          { share = 3, panels = { { type = "boxes.one" } } },
          { size = 260, panels = { { type = "boxes.two" } } },
        },
      },
    },
    { name = "Two", windows = { { panels = { { type = "cboxes.box" } } } } },
    { name = "Three",
      windows = {
        { split = "vertical",
          { share = 3, split = "horizontal",
            { panels = { { type = "boxes.one" } } },
            { panels = { { type = "cboxes.box" } } },
          },
          { share = 1, panels = { { type = "boxes.two" }, { type = "boxes.three" } } },
        },
      },
    },
  },
}
