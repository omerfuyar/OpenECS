-- a pad that types while it takes text input, and binds keys with and without Ctrl; what reached it is kept in the plugin state

local ecs = require("ecs")

local results = { keys = {}, text = "", bound = 0, ctrlBound = 0 }
local pad

ecs.panel.registerType({
  name = "typist.pad",
  title = "Pad",
  create = function(panel)
    pad = panel
    panel:setTextInput(true)
    return {}
  end,
  event = function(_, event)
    if event.type == "keyDown" then
      results.keys[#results.keys + 1] = event.key
    elseif event.type == "text" then
      results.text = results.text .. event.text
    end
  end,
})

assert(ecs.service.register("typist", {
  bound = { sig = "void()", doc = "Counts a plain bound key", fn = function() results.bound = results.bound + 1 end },
  ctrlBound = { sig = "void()", doc = "Counts a bound key with Ctrl", fn = function() results.ctrlBound = results.ctrlBound + 1 end },
  stop = { sig = "void()", doc = "Stops taking text input", fn = function() pad:setTextInput(false) end },
}))

assert(ecs.input.bind("typist.pad", "A", "typist.bound"))
assert(ecs.input.bind("typist.pad", "Ctrl+B", "typist.ctrlBound"))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
