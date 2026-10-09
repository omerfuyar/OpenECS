---@type ecs.Manifest
return {
  name = "listener",
  version = "0.1.0",
  api = 1,
  description = "Panels that count the ticks of ticker and the panels that open",
  depends = { ticker = "0.1" }, -- a plugin hears only the core's events, its own, and those of the plugins it depends on
  lua = "init.lua",
}
