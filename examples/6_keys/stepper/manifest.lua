---@type ecs.Manifest
return {
  name = "stepper",
  version = "0.1.0",
  api = 1,
  description = "A number that keys step up and down",
  depends = { ui = "0.1" },
  lua = "init.lua",
}
