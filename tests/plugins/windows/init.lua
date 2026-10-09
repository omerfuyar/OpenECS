-- opens and moves panels into pop-out windows and back, and keeps what it saw in its plugin state

local ecs = require("ecs")

local results = {}

local function run()
  local one = assert(ecs.layout.find(1))
  local two = assert(ecs.layout.find(2))

  -- a new panel in a new window, and a moved one
  results.opened = ecs.layout.open("boxes.three", nil, nil, "window") ~= nil
  results.moved = ecs.layout.move(one, nil, "window")

  -- a move needs a target, except into a new window
  results.refused = not pcall(ecs.layout.move, two, nil, "center")

  -- a panel alone in its window stays
  results.stayed = ecs.layout.move(one, nil, "window")
end

-- moves the first panel back beside the second, in the main window
local function back()
  assert(ecs.layout.move(assert(ecs.layout.find(1)), assert(ecs.layout.find(2)), "right"))
end

assert(ecs.service.register("windows", {
  run = { sig = "void()", doc = "Opens and moves panels into pop-out windows", fn = run },
  back = { sig = "void()", doc = "Moves the first panel back to the main window", fn = back },
}))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
