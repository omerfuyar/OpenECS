-- calls each of boxes and cboxes with Lua functions, and saves what it saw in its plugin state

local ecs = require("ecs")
assert(require("string") == string, "require gives Lua's modules for other names")

local SIGNATURE = "int(fn<void(handle<ecs.panel>, int)>)"
local results = {}

local function each(plugin)
  local each_ = assert(ecs.service.get(plugin .. ".each", SIGNATURE))
  local seen = {}
  local count = each_(function(panel, clicks)
    assert(ecs.panel.getType(panel) == panel:getType() and ecs.panel.getId(panel) == panel:getId())
    seen[#seen + 1] = { type = panel:getType(), clicks = clicks }
  end)
  return { count = count, seen = seen, without = each_(nil) }
end

local function run()
  results.c = each("cboxes")
  results.lua = each("boxes")

  -- a callback is a parameter only, and its signature has no callbacks
  local refused = {}
  for _, signature in ipairs({ "fn<void()>()", "void(out fn<void()>)", "void(fn<void(fn<void()>)>)", "void(fn<void(>)" }) do
    refused[#refused + 1] = ecs.service.register("callbacks", { bad = { sig = signature, fn = function() end } }) == nil
  end
  results.refused = refused
end

assert(ecs.service.register("callbacks", { run = { sig = "void()", doc = "Calls each of boxes and cboxes", fn = run } }))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
