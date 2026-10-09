-- A palette whose colours drag onto samples. A sample also takes files dropped from other applications, and shows their names.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

local COLORS = { 0xFFBF616A, 0xFFD08770, 0xFFEBCB8B, 0xFFA3BE8C, 0xFF5E81AC, 0xFFB48EAD }
local SWATCH = 48 -- in layout units

-- the palette: one row of squares
ecs.panel.registerType({
  name = "swatches.palette",
  title = "Palette",

  create = function(panel)
    return { panel = panel, scale = 1 }
  end,

  draw = function(palette, surface)
    palette.scale = surface.scale
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, color("background"))

    for i, argb in ipairs(COLORS) do
      fill(surface, (i - 1) * SWATCH, 0, SWATCH, SWATCH, argb)
    end
  end,

  event = function(palette, event)
    if event.type ~= "pointerDown" or event.y / palette.scale >= SWATCH then
      return
    end

    local argb = COLORS[math.floor(event.x / palette.scale / SWATCH) + 1]

    if argb then
      -- call it while the button that was pressed on this panel is held; the type names what kind of value it carries
      palette.panel:startDrag("swatches.color", argb)
    end
  end,
})

-- the sample: one colour, and the names of the files dropped last
ecs.panel.registerType({
  name = "swatches.sample",
  title = "Sample",
  stateVersion = 1,

  create = function(panel, saved)
    -- the types the panel takes; panels that take the dragged type are outlined during a drag
    panel:acceptDrops({ "swatches.color", "file-list" })
    return { panel = panel, color = saved and saved.color or 0xFF4C566A, files = {} }
  end,

  draw = function(sample, surface)
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, sample.color)

    for i, name in ipairs(sample.files) do
      text(surface, name, 16, 16 + (i - 1) * 24, 16, color("text"))
    end
  end,

  event = function(sample, event)
    if event.type ~= "drop" then
      return
    end

    if event.dataType == "swatches.color" then
      sample.color = event.value
    else
      -- a "file-list" is a list of paths
      sample.files = {}

      for _, path in ipairs(event.value) do
        table.insert(sample.files, path:match("[^/]*$"))
      end
    end

    sample.panel:redraw()
  end,

  saveState = function(sample)
    return { color = sample.color, files = sample.files }
  end,
})
