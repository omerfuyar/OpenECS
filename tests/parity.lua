-- every public C function names its Lua counterparts with @lua in OpenECS.h, every function of ecs.lua is named there or says it is Lua only or keys can run it,
-- and every function the core's settings file binds to a key says keys can run it (DESIGN 11.5)

local folder = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"

local function read(path)
  local file = assert(io.open(folder .. path, "r"))
  local text = file:read("a")
  file:close()
  return text
end

-- the functions of ecs.lua, with their documentation; a method is named by its class, such as panel:getTitle, and a function field of a table of the module by its path
local function readLua()
  local functions = {}
  local doc = ""
  local class = nil

  for line in read("../include/ecs.lua"):gmatch("[^\n]*") do
    local name = line:match("^function ([%w.]+)%(")
    local owner, method = line:match("^function (%w+):(%w+)%(")
    local field, fieldDoc = line:match("^%-%-%-@field (%w+) fun%(.-%) (.*)")
    class = line:match("^%-%-%-@class (%S+)") or class
    name = owner and owner:sub(1, 1):lower() .. owner:sub(2) .. ":" .. method or name

    if name then
      functions[name] = doc
    elseif field and (class == "ecs" or class:match("^ecs%.%l")) then
      functions[class .. "." .. field] = fieldDoc
    end

    doc = line:match("^%-%-%-") and doc .. line or ""
  end

  return functions
end

return {
  preset = "presets/sketch.lua",
  run = function()
    local functions = readLua()
    local named = {}
    local problems = {}
    local counterparts = nil

    -- an @lua line is the last line of the documentation of the function after it
    for line in read("../include/OpenECS.h"):gmatch("[^\n]*") do
      local function_ = line:match("^OPENECS_EXPORT.-%f[%w](ECS%w*_%w+)%(")

      if line:match("^/// @lua ") then
        counterparts = line:match("^/// @lua (.*)")
      elseif function_ then
        -- values have no Lua functions, because Lua passes its own values
        if counterparts == nil and not function_:match("^ECSValue_") then
          problems[#problems + 1] = function_ .. " has no @lua line"
        end

        if counterparts ~= nil and not counterparts:match("^none: ") then
          for name in counterparts:gmatch("[^,%s]+") do
            named[name] = true

            if not functions[name] then
              problems[#problems + 1] = function_ .. " names " .. name .. ", which ecs.lua does not have"
            end
          end
        end

        counterparts = nil
      end
    end

    for name, doc in pairs(functions) do
      if not named[name] and not doc:find("Lua only:", 1, true) and not doc:find("Keys can run it.", 1, true) then
        problems[#problems + 1] = name .. " of ecs.lua is named by no @lua line, and does not say it is Lua only or keys can run it"
      end
    end

    -- the core's settings file binds keys to the core's functions
    local prefixKeys = dofile(folder .. "../resources/settings.lua")["ecs.prefixKeys"]

    for key, name in pairs(prefixKeys) do
      if not (functions[name] or ""):find("Keys can run it.", 1, true) then
        problems[#problems + 1] = "the key " .. key .. " runs " .. name .. ", which ecs.lua does not have as a function keys can run"
      end
    end

    table.sort(problems)
    assert(#problems == 0, "\n" .. table.concat(problems, "\n"))
  end,
}
