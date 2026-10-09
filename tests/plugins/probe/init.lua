-- keeps the values in effect of the settings a test changes, read each time the session is built

local ecs = require("ecs")

ecs.plugin.registerState({
  version = 1,
  save = function()
    return { focus = ecs.settings.get("ecs.focus"), reopenLimit = ecs.settings.get("ecs.reopenLimit"), accent = ecs.settings.get("ecs.colorAccent") }
  end,
  restore = function() end,
})
