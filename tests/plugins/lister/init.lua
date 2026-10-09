-- lists the presets and the saved sessions, tries to open a preset, and keeps what it got in its plugin state

local ecs = require("ecs")

local results = {}

local function list()
  results.presets = ecs.session.presets()
  results.sessions = ecs.session.sessions()
  results.opened, results.message = ecs.session.openPreset("default")
  results.opened = results.opened or false
end

assert(ecs.service.register("lister", { list = { sig = "void()", doc = "Lists presets and sessions, and tries to open a preset", fn = list } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
