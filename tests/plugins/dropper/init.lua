-- boxes that accept the types their saved state names; a box whose state says drags starts dragging a colour when it is pressed
-- what the boxes get is kept in the plugin state

local ecs = require("ecs")

local results = { drops = {}, releases = 0 }

ecs.panel.registerType({
  name = "dropper.box",
  title = "Box",
  create = function(panel, saved)
    panel:acceptDrops(saved and saved.accepts or {})
    return { panel = panel, drags = saved and saved.drags }
  end,
  event = function(state, event)
    if event.type == "pointerDown" and state.drags then
      results.started = state.panel:startDrag("color", { name = "red", argb = 0xFFFF0000 })
    elseif event.type == "pointerUp" and state.drags then
      results.releases = results.releases + 1
    elseif event.type == "drop" then
      results.drops[#results.drops + 1] = { panel = state.panel:getId(), dataType = event.dataType, value = event.value, inside = event.x > 0 and event.y > 0 }
    end
  end,
})

-- starting a drag without a pressed button fails
local function tryDrag(panel)
  local ok, message = panel:startDrag("color")
  results.refused = ok == nil and message ~= nil
end

assert(ecs.service.register("dropper", { tryDrag = { sig = "void(handle<ecs.panel>)", doc = "Starts a drag without a pressed button", fn = tryDrag } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
