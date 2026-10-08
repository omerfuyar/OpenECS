-- saves the session to a temporary file, and keeps the file's path and the result in its plugin state

local ecs = require("ecs")

local results = {}

local function save()
  results.path = os.tmpname()
  results.saved, results.message = ecs.session.save(results.path)
end

assert(ecs.service.register("saver", { save = { sig = "void()", doc = "Saves the session to a temporary file", fn = save } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
