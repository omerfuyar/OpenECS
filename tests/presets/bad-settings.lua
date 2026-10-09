-- one box, and settings whose values do not have their types: they are reported, and the defaults are used
return {
  format = 1,
  name = "bad-settings",
  version = "0.1.0",
  app = { id = "openecs.test.badSettings", name = "Bad settings" },
  depends = { boxes = "0.1", probe = "0.1" },
  pluginsDir = "../plugins",
  settings = { ["ecs.colorAccent"] = "blue", ["ecs.prefix"] = "Hyper+Q", ["ecs.focus"] = "always" },
  workspaces = {
    { name = "One", windows = { { panels = { { type = "boxes.one" } } } } },
  },
}
