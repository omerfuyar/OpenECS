---@type ecs.Manifest
return {
  name = "picker",
  version = "0.1.0",
  api = 1,
  description = "A panel whose colour a right-click menu chooses",
  depends = { ui = "0.1" },
  lua = "init.lua",
}
