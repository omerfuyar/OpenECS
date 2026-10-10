-- keeps the values in effect of the settings a test changes, read each time the session is built

local ecs = require("ecs")

-- a list, which the settings window types as a Lua table
assert(ecs.settings.declare({ name = "probe.sizes", type = "list", description = "Sizes", default = { 1, 2 } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return { focus = ecs.settings.get("ecs.focus"), reopenLimit = ecs.settings.get("ecs.reopenLimit"), accent = ecs.settings.get("ecs.colorAccent"), sizes = ecs.settings.get("probe.sizes") }
  end,
  restore = function() end,
})
