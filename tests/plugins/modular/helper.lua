-- a module in the plugin's folder: it runs once, and gets the plugin's own ecs table

local ecs = require("ecs")

local helper = { runs = 0 }
helper.runs = helper.runs + 1

-- a global of a module stays in the plugin's environment
modularGlobal = "set"

function helper.name()
  return ecs.plugin.name
end

return helper
