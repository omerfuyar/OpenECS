-- every public C function names its Lua counterparts with @lua in OpenECS.h, and every function of ecs.lua is named there or says it is Lua only (DESIGN 11.5)

local folder = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"

local function read(path)
  local file = assert(io.open(folder .. path, "r"))
  local text = file:read("a")
  file:close()
  return text
end

-- the functions of ecs.lua, and those whose documentation says "Lua only"; a method is named by its class, such as panel:getTitle
local function readLua()
  local functions, luaOnly = {}, {}
  local doc = ""

  for line in read("../include/ecs.lua"):gmatch("[^\n]*") do
    local name = line:match("^function ([%w.]+)%(")
    local class, method = line:match("^function (%w+):(%w+)%(")
    name = class and class:sub(1, 1):lower() .. class:sub(2) .. ":" .. method or name

    if name then
      functions[name] = true
      luaOnly[name] = doc:find("Lua only:", 1, true) ~= nil
    end

    doc = line:match("^%-%-%-") and doc .. line or ""
  end

  return functions, luaOnly
end

return {
  preset = "presets/sketch.lua",
  run = function()
    local functions, luaOnly = readLua()
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

    for name in pairs(functions) do
      if not named[name] and not luaOnly[name] then
        problems[#problems + 1] = name .. " of ecs.lua is named by no @lua line, and does not say it is Lua only"
      end
    end

    table.sort(problems)
    assert(#problems == 0, "\n" .. table.concat(problems, "\n"))
  end,
}
