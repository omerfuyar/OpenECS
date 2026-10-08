-- saves the session to a temporary file and tries to open it, and keeps the file's path and the results in its plugin state

local ecs = require("ecs")

local results = {}

local function save()
  results.path = os.tmpname()
  results.saved, results.message = ecs.session.save(results.path)
  results.opened = ecs.session.open(results.path) or false
end

assert(ecs.service.register("saver", { save = { sig = "void()", doc = "Saves the session to a temporary file and tries to open it", fn = save } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
