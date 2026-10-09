---@type ecs.Manifest
return {
  name = "greeter",
  version = "0.1.0",
  api = 1,
  description = "A greeting whose words, colour and size are settings",
  depends = { ui = "0.1" },
  lua = "init.lua",
}
