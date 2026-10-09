-- a board that draws with the ui plugin, then reads its own pixels to tell what ui drew; the results are kept in the plugin state

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill"))
local text = assert(ecs.service.get("ui.text"))
local measure = assert(ecs.service.get("ui.measure"))
local color = assert(ecs.service.get("ui.color"))
local outline = assert(ecs.service.get("ui.outline"))
local image = assert(ecs.service.get("ui.image"))
local imageSize = assert(ecs.service.get("ui.imageSize"))
local check = assert(ecs.service.get("ui.check"))
local field = assert(ecs.service.get("ui.field"))
local scrollbar = assert(ecs.service.get("ui.scrollbar"))
local thumb = assert(ecs.service.get("ui.thumb"))

-- the plugin's own image, a green square
local GREEN = ecs.plugin.folder .. "green.png"

local results = {}
local kept

-- counts the pixels of a rectangle that are not black
local function inked(surface, x, y, width, height)
  local count = 0

  for row = y, y + height - 1 do
    for column = x, x + width - 1 do
      if surface:getPixel(column, row) ~= 0xFF000000 then
        count = count + 1
      end
    end
  end

  return count
end

ecs.panel.registerType({
  name = "painter.board",
  title = "Board",
  create = function()
    return {}
  end,
  draw = function(_, surface)
    fill(surface, 0, 0, surface.width, surface.height, 0xFF000000)
    fill(surface, 10, 10, 20, 20, 0xFFFF0000)
    fill(surface, 40, 10, 20, 20, 0x80FFFFFF)
    fill(surface, -10, -10, 5, 5, 0xFFFFFFFF)

    local width = text(surface, "Hello", 10, 50, 16, 0xFFFFFFFF)
    local measuredWidth, lineHeight = measure("Hello", 16)

    results.red = surface:getPixel(20, 20)
    results.blended = surface:getPixel(50, 20)
    results.corner = surface:getPixel(0, 0)
    results.width = width
    results.measured = measuredWidth == width and lineHeight > 0
    results.inked = inked(surface, 10, 50, math.ceil(width), math.ceil(lineHeight)) > 0
    results.outside = inked(surface, 100, 50, 20, 20)

    -- an outline draws only the edge, and an image is stretched to its rectangle
    outline(surface, 10, 80, 20, 20, 2, 0xFFFFFFFF)
    results.edge = surface:getPixel(10, 90)
    results.middle = surface:getPixel(20, 90)
    results.imageDrawn = image(surface, GREEN, 40, 80, 20, 20)
    results.green = surface:getPixel(50, 90)
    results.missing = image(surface, "nope.png", 40, 80, 20, 20)
    results.size = { imageSize(GREEN) }

    -- a check box is filled when it is on, a field gives its cursor's x after its text, and a scrollbar draws its thumb
    check(surface, 70, 80, 20, true)
    results.checked = surface:getPixel(80, 90) == color("accent")
    results.cursor = field(surface, "Hi", 100, 80, 200, 24, true) > 100
    scrollbar(surface, 310, 80, 8, 100, 400, 100, 0)
    results.thumb = surface:getPixel(314, 85) == color("tabShown") and surface:getPixel(314, 170) == color("tabRow")
    results.thumbPlace = { thumb(100, 400, 100, 300) }

    -- the core's own fill replaces pixels, in pixels, and clips
    surface:fill(-5, 120, 15, 10, 0xFF0000FF)
    results.surfaceFill = surface:getPixel(5, 125)
    kept = surface
  end,
})

-- a surface handle is valid only while draw runs
local function useKept()
  results.kept = pcall(function()
    return kept.width
  end)
end

local function readColors()
  results.colors = { color("#102030"), color("#10203080"), color("background"), color("nope") }
  results.background = ecs.settings.get("ecs.colorBackground")
end

assert(ecs.service.register("painter", {
  useKept = { sig = "void()", doc = "Uses a surface handle after draw returned", fn = useKept },
  readColors = { sig = "void()", doc = "Reads colours with ui.color", fn = readColors },
}))

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
