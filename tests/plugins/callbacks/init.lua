-- calls the twins' each_canvas with Lua functions, and saves what it saw in its plugin state
local SIGNATURE = "int(fn<void(handle<ecs.panel>, int)>)"
local results = {}

local function each(plugin)
  local each_canvas = assert(ecs.service.get(plugin .. ".each_canvas", SIGNATURE))
  local seen = {}
  local count = each_canvas(function(panel, strokes)
    seen[#seen + 1] = { type = panel:get_type(), strokes = strokes }
  end)
  return { count = count, seen = seen, without = each_canvas(nil) }
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

assert(ecs.service.register("callbacks", { run = { sig = "void()", doc = "Calls the twins' each_canvas", fn = run } }))

ecs.plugin.register_state({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
