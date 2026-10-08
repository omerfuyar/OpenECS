-- calls the twins' eachCanvas with Lua functions, and saves what it saw in its plugin state

local ecs = require("ecs")
assert(require("string") == string, "require gives Lua's modules for other names")

local SIGNATURE = "int(fn<void(handle<ecs.panel>, int)>)"
local results = {}

local function each(plugin)
  local eachCanvas = assert(ecs.service.get(plugin .. ".eachCanvas", SIGNATURE))
  local seen = {}
  local count = eachCanvas(function(panel, strokes)
    assert(ecs.panel.getType(panel) == panel:getType() and ecs.panel.getId(panel) == panel:getId())
    seen[#seen + 1] = { type = panel:getType(), strokes = strokes }
  end)
  return { count = count, seen = seen, without = eachCanvas(nil) }
end

local function run()
  results.c = each("sketch_c")
  results.lua = each("sketch_lua")

  -- a callback is a parameter only, and its signature has no callbacks
  local refused = {}
  for _, signature in ipairs({ "fn<void()>()", "void(out fn<void()>)", "void(fn<void(fn<void()>)>)", "void(fn<void(>)" }) do
    refused[#refused + 1] = ecs.service.register("callbacks", { bad = { sig = signature, fn = function() end } }) == nil
  end
  results.refused = refused
end

assert(ecs.service.register("callbacks", { run = { sig = "void()", doc = "Calls the twins' eachCanvas", fn = run } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
