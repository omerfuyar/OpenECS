-- three boxes side by side: the first drags colours and accepts colours, files and text, the second accepts colours, the third nothing
return {
  format = 1,
  name = "dropper",
  version = "0.1.0",
  app = { id = "openecs.test.dropper", name = "Dropper" },
  depends = { dropper = "0.1" },
  pluginsDir = "../plugins",
  workspaces = {
    { name = "Boxes",
      windows = {
        { split = "horizontal",
          { panels = { { type = "dropper.box", state = { drags = true, accepts = { "color", "file-list", "text" } } } } },
          { panels = { { type = "dropper.box", state = { accepts = { "color" } } } } },
          { panels = { { type = "dropper.box", state = {} } } },
        },
      },
    },
  },
}
