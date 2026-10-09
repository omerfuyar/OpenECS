---@type ecs.Manifest
return {
  name = "button",
  version = "0.1.0",
  api = 1,
  description = "A button that counts clicks, drawn with the ui plugin",
  depends = { ui = "0.1" }, -- ui loads first, so its functions exist when init.lua runs
  lua = "init.lua",
}
