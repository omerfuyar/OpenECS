-- requires its own modules and keeps what it found in its plugin state

local ecs = require("ecs")
local helper = require("helper")
local shapes = require("parts.shapes")

local broken, message = pcall(require, "broken")
local missing = pcall(require, "nothing.here")

local results = {
  name = helper.name(),
  shapes = shapes.count,
  shared = shapes.helper == helper,
  once = require("helper") == helper and helper.runs == 1,
  tools = require("tools"),
  global = modularGlobal == "set" and rawget(_G, "modularGlobal") == nil,
  broken = broken,
  message = tostring(message):match("broken on purpose") ~= nil,
  missing = missing,
}

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
