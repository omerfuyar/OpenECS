---@type ecs.Manifest
return {
  name = "tally",
  version = "0.1.0",
  api = 1,
  description = "Panels that count clicks with the counter plugin",
  depends = { counter = "0.1", ui = "0.1" },
  lua = "init.lua",
}
