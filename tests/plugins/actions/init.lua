-- runs the core's functions from Lua as keys run them, and keeps what it saw in its plugin state

local ecs = require("ecs")

local results = {}

local function run()
  results.types = {
    maximize = type(ecs.layout.maximize),
    restart = type(ecs.panel.restart),
    switch1 = type(ecs.workspace.switch1),
    moveToWorkspace10 = type(ecs.layout.moveToWorkspace10),
  }

  -- without a panel, closing is the user's close, which a lock stops
  ecs.layout.lock()
  results.closedLocked = ecs.layout.close()
  ecs.layout.lock()

  ecs.layout.maximize()
  ecs.workspace.switch2()
end

assert(ecs.service.register("actions", { run = { sig = "void()", doc = "Runs the core's functions from Lua", fn = run } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
