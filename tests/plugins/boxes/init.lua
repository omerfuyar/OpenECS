-- Panels for the tests: boxes of one colour. A box counts the clicks on it, and keeps the lines of the file it was opened with.

local ecs = require("ecs")

local COLORS = { one = 0xFF5E81AC, two = 0xFFA3BE8C, three = 0xFFEBCB8B }

local boxes = {} -- every open box, by its panel handle

for kind, color in pairs(COLORS) do
  ecs.panel.registerType({
    name = "boxes." .. kind,
    title = "Box " .. kind,
    stateVersion = 1,
    create = function(panel, saved)
      local box = { panel = panel, clicks = saved and saved.clicks or 0, lines = saved and saved.lines or {} }
      boxes[panel] = box
      return box
    end,
    destroy = function(box)
      boxes[box.panel] = nil
    end,
    draw = function(_, surface)
      local row = string.pack("=I4", color):rep(surface.width)

      for y = 0, surface.height - 1 do
        surface:setRow(y, row)
      end
    end,
    event = function(box, event)
      if event.type == "pointerDown" then
        box.clicks = box.clicks + 1
      end
    end,
    saveState = function(box)
      return { clicks = box.clicks, lines = box.lines }
    end,
  })
end

local services = {}

-- opens a box with the lines of a file; the function the test presets open files with
function services.open(path)
  local file = io.open(path, "r")
  local panel = file and ecs.layout.open("boxes.one")

  if not file or not panel then
    ecs.log.warn(("Cannot open '%s'."):format(path))
    return
  end

  for line in file:lines() do
    table.insert(boxes[panel].lines, line)
  end

  file:close()
end

-- calls a function with every box and its clicks
function services.each(fn)
  local count = 0

  for panel, box in pairs(boxes) do
    if fn then
      fn(panel, box.clicks)
    end

    count = count + 1
  end

  return count
end

assert(ecs.service.register("boxes", {
  open = { sig = "void(string)", doc = "Open a box with the lines of a file", fn = services.open },
  each = { sig = "int(fn<void(handle<ecs.panel>, int)>)", doc = "Calls a function with every box and its clicks", fn = services.each },
}))
