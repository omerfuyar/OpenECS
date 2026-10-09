-- A button that counts its clicks. It draws with the ui standard plugin, and follows the pointer.
-- A standard plugin ships with OpenECS for other plugins to build on; its manifest is in plugins/ui/.

local ecs = require("ecs")

-- another plugin's functions, its services, are looked up by name and signature; get them once, while the plugin loads
-- a signature is the result type, then the argument types: handle<ecs.surface> is the surface draw gets, int64 a colour,
-- and an out argument comes back as an extra result
local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local measure = assert(ecs.service.get("ui.measure", "void(string, float, out float, out float)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

-- layout units keep sizes the same on every screen; a surface's scale says how many pixels one layout unit is
local WIDTH, HEIGHT, TEXT_SIZE = 200, 60, 18

-- the button's rectangle, centred in a panel of a size in layout units
local function buttonRect(width, height)
  return (width - WIDTH) / 2, (height - HEIGHT) / 2
end

local function inside(button, x, y)
  local left, top = buttonRect(button.width, button.height)
  return x >= left and x < left + WIDTH and y >= top and y < top + HEIGHT
end

ecs.panel.registerType({
  name = "button.panel",
  title = "Button",

  create = function(panel)
    return { panel = panel, clicks = 0, hover = false, width = 0, height = 0, scale = 1 }
  end,

  draw = function(button, surface)
    -- the surface is in pixels; ui works in layout units, so divide by the scale
    button.scale = surface.scale
    button.width, button.height = surface.width / surface.scale, surface.height / surface.scale
    local left, top = buttonRect(button.width, button.height)

    fill(surface, 0, 0, button.width, button.height, color("background")) -- a colour of the core's theme, which settings change
    fill(surface, left, top, WIDTH, HEIGHT, color(button.hover and "#88C0D0" or "#5E81AC"))

    local label = ("Clicked %d times"):format(button.clicks)
    local labelWidth, lineHeight = measure(label, TEXT_SIZE) -- out parameters come back as results
    text(surface, label, left + (WIDTH - labelWidth) / 2, top + (HEIGHT - lineHeight) / 2, TEXT_SIZE, color("text"))
  end,

  -- pointer positions are in surface pixels
  event = function(button, event)
    if event.type == "pointerMove" then
      local hover = inside(button, event.x / button.scale, event.y / button.scale)

      if hover ~= button.hover then
        button.hover = hover
        button.panel:redraw() -- the core draws a panel only when asked, or every frame if its type is continuous
      end
    elseif event.type == "pointerDown" and event.button == 1 and inside(button, event.x / button.scale, event.y / button.scale) then
      button.clicks = button.clicks + 1
      button.panel:setTitle(("Button (%d)"):format(button.clicks))
      button.panel:redraw()
    end
  end,
})
