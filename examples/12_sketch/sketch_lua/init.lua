-- Sketch in Lua: canvases to draw on with the mouse, and a clock. The sketch_c plugin does the same in C, so the two can be compared.

local ecs = require("ecs")

local NAME = ecs.plugin.name

-- makes a name of this plugin, such as "sketch_lua.canvas"
local function name(localName)
  return NAME .. "." .. localName
end

-- colours a brush can have, in ARGB8888, and their names in the setting sketch_lua.brushColor
local COLOR_NAMES = { "white", "red", "green", "blue", "yellow" }
local COLORS = { white = 0xFFECEFF4, red = 0xFFBF616A, green = 0xFFA3BE8C, blue = 0xFF5E81AC, yellow = 0xFFEBCB8B }
local BACKGROUND = 0xFF2E3440
local CLOCK_HAND = 0xFF88C0D0

-- the clipboard type of strokes; both sketch plugins use it, so strokes copied in one paste into the other
local CLIPBOARD_TYPE = "application/x-openecs-strokes"

-- version of the saved state of a canvas, and of the plugin's own state
local STATE_VERSION = 1

local MIN_SIZE, MAX_SIZE = 1, 64

local canvases = {}       -- every open canvas's state, by its panel handle
local openCanvases = 0   -- counted from the core's events
local total, saves = 0, 0 -- strokes drawn and canvases saved in every session; the plugin's own state

local function clamp(value, low, high)
  return math.max(low, math.min(high, value))
end

-- strokes ---------------------------------------------------------------

-- paints strokes by calling plot(x, y, color) for each pixel: dabs at every point, and between points every half brush
local function paint(width, height, strokes, plot)
  for _, stroke in ipairs(strokes) do
    local radius = stroke.size / 2
    local step = stroke.size > 2 and stroke.size / 2 or 1
    local points = stroke.points

    local function dab(cx, cy)
      for y = math.max(0, math.floor(cy - radius)), math.min(height - 1, math.ceil(cy + radius)) do
        for x = math.max(0, math.floor(cx - radius)), math.min(width - 1, math.ceil(cx + radius)) do
          local dx, dy = x + 0.5 - cx, y + 0.5 - cy

          if dx * dx + dy * dy <= radius * radius then
            plot(x, y, stroke.color)
          end
        end
      end
    end

    for p = 1, #points - 1, 2 do
      local x, y = points[p], points[p + 1]
      dab(x, y)

      if p + 3 <= #points then
        local dx, dy = points[p + 2] - x, points[p + 3] - y
        local steps = math.floor(math.sqrt(dx * dx + dy * dy) / step)

        for k = 1, steps - 1 do
          dab(x + dx * k / steps, y + dy * k / steps)
        end
      end
    end
  end
end

-- writes strokes as text, one stroke per line: "size color x y x y ..."; the clipboard uses it
local function strokesToText(strokes)
  local lines = {}

  for _, stroke in ipairs(strokes) do
    local line = { stroke.size, stroke.color }

    for _, number in ipairs(stroke.points) do
      line[#line + 1] = ("%.1f"):format(number)
    end

    lines[#lines + 1] = table.concat(line, " ") .. "\n"
  end

  return table.concat(lines)
end

-- reads strokes written by strokesToText, and adds them to a canvas; gives how many it read
local function strokesFromText(canvas, text)
  local added = 0

  for line in text:gmatch("[^\n]+") do
    local numbers = {}

    for number in line:gmatch("%S+") do
      numbers[#numbers + 1] = tonumber(number)
    end

    local size, color = numbers[1], numbers[2]

    if size and color and size >= MIN_SIZE and size <= MAX_SIZE then
      local stroke = { size = math.floor(size), color = math.floor(color), points = {} }

      for i = 3, #numbers - 1, 2 do
        stroke.points[#stroke.points + 1] = numbers[i]
        stroke.points[#stroke.points + 1] = numbers[i + 1]
      end

      table.insert(canvas.strokes, stroke)
      added = added + 1
    end
  end

  return added
end

-- reads a whole file; nil if it cannot be read
local function readFile(path)
  local file = io.open(path, "rb")
  local text = file and file:read("a")

  if file then
    file:close()
  end

  return text
end

-- canvas ----------------------------------------------------------------

local function findCanvas(panel)
  local canvas = canvases[panel]

  if not canvas then
    ecs.log.warn("That panel is not a sketch_lua canvas.")
  end

  return canvas
end

local function settingSize()
  return clamp(ecs.settings.get(name("brushSize")) or 6, MIN_SIZE, MAX_SIZE)
end

local function settingColor()
  return COLORS[ecs.settings.get(name("brushColor"))] or COLORS.white
end

-- shows the number of strokes in the canvas's title
local function canvasTitle(canvas)
  canvas.panel:setTitle(("Lua canvas (%d)"):format(#canvas.strokes))
end

-- marks a canvas changed: unsaved, retitled and redrawn
local function canvasChanged(canvas)
  canvas.unsaved = true
  canvas.panel:setUnsaved(true)
  canvasTitle(canvas)
  canvas.panel:redraw()
end

-- tells subscribers that a canvas has a new stroke
local function emitStroke(canvas)
  ecs.event.emit(name("strokeAdded"), { panel = canvas.panel:getId(), strokes = #canvas.strokes })
end

-- adds dropped strokes to a canvas: strokes or text in the format of copy, or the strokes of each dropped file
local function dropStrokes(canvas, dataType, value)
  local added = 0

  if dataType == "file-list" then
    for _, path in ipairs(value) do
      added = added + strokesFromText(canvas, readFile(path) or "")
    end
  else
    added = strokesFromText(canvas, value)
  end

  ecs.log.info(("Dropped %d strokes."):format(added))

  if added > 0 then
    canvasChanged(canvas)
  end
end

-- redraws every canvas when a brush setting changes
local function brushChanged()
  for _, canvas in pairs(canvases) do
    canvas.panel:redraw()
  end

  ecs.log.debug(("The brush is %d, %s."):format(settingSize(), ecs.settings.get(name("brushColor"))))
end

ecs.settings.declare({
  name = name("brushSize"),
  type = "integer",
  description =
  "Size of the brush, in pixels; the wheel over a canvas changes it",
  default = 6,
  changed = brushChanged
})
ecs.settings.declare({
  name = name("brushColor"),
  type = "choice",
  description = "Colour of the brush",
  default =
  "white",
  choices = COLOR_NAMES,
  changed = brushChanged
})
ecs.settings.declare({
  name = name("reminderSeconds"),
  type = "number",
  description =
  "How often a canvas with unsaved strokes says so",
  default = 30.0
})
ecs.panel.registerType({
  name = name("canvas"),
  title = "Lua canvas",
  stateVersion = STATE_VERSION,
  minWidth = 64,
  minHeight = 64,

  create = function(panel, saved, version)
    local canvas = { panel = panel, strokes = {}, drawing = false, unsaved = false, width = 0, height = 0 }

    -- strokes = { { size = 6, color = 0xFFECEFF4, points = { x, y, ... } }, ... }; older versions are not read
    if version == STATE_VERSION and saved and saved.strokes then
      for _, stroke in ipairs(saved.strokes) do
        table.insert(canvas.strokes,
          { size = stroke.size or 6, color = stroke.color or COLORS.white, points = stroke.points or {} })
      end
    end

    canvases[panel] = canvas
    canvasTitle(canvas)

    -- a canvas takes strokes dragged from a canvas, text in their format, and files of it
    panel:acceptDrops({ CLIPBOARD_TYPE, "text", "file-list" })

    -- a panel timer, so it stops when the canvas closes
    panel:startTimer(math.max(1, ecs.settings.get(name("reminderSeconds")) or 30), true, function()
      if canvas.unsaved then
        ecs.log.info(panel:getTitle() .. " has unsaved strokes.")
      end
    end)

    return canvas
  end,

  destroy = function(canvas)
    canvases[canvas.panel] = nil
  end,

  draw = function(canvas, surface)
    canvas.width, canvas.height = surface.width, surface.height
    surface:fill(0, 0, surface.width, surface.height, BACKGROUND)

    paint(surface.width, surface.height, canvas.strokes, function(x, y, color)
      surface:setPixel(x, y, color)
    end)
  end,

  event = function(canvas, event)
    local panel = canvas.panel

    if event.type == "pointerDown" and event.button == 1 and event.shift then
      -- Shift and a press drag the canvas's strokes, in the text format of copy
      if not panel:startDrag(CLIPBOARD_TYPE, strokesToText(canvas.strokes)) then
        ecs.log.warn("Cannot drag the strokes.")
      end
    elseif event.type == "pointerDown" and event.button == 1 then
      local brush = canvas.brush
      table.insert(canvas.strokes,
        { size = brush and brush.size or settingSize(), color = brush and brush.color or settingColor(), points = { event.x, event.y } })
      canvas.drawing = true
      panel:redraw()
    elseif event.type == "pointerMove" and canvas.drawing then
      local points = canvas.strokes[#canvas.strokes].points
      points[#points + 1] = event.x
      points[#points + 1] = event.y
      panel:redraw()
    elseif event.type == "pointerUp" and canvas.drawing then
      canvas.drawing = false
      canvasChanged(canvas)
      emitStroke(canvas)
    elseif event.type == "wheel" then
      -- the wheel changes the setting, so every canvas and the settings window see the new size
      local ok = ecs.settings.set(name("brushSize"),
        clamp(settingSize() + (event.wheelY > 0 and 1 or -1), MIN_SIZE, MAX_SIZE))

      if not ok then
        ecs.log.warn("Cannot change the brush size.")
      end
    elseif event.type == "focused" or event.type == "unfocused" then
      ecs.log.debug(("%s is %s."):format(panel:getTitle(), event.type))
    elseif event.type == "shown" or event.type == "resized" then
      ecs.log.debug(("%s is %s at %.0fx%.0f."):format(panel:getTitle(), event.type, event.width, event.height))
    elseif event.type == "hidden" then
      ecs.log.debug(panel:getTitle() .. " is hidden.")
    elseif event.type == "drop" then
      dropStrokes(canvas, event.dataType, event.value)
    end
  end,

  saveState = function(canvas)
    return { strokes = canvas.strokes }
  end,

  -- saves a canvas's unsaved work; its strokes are already in its saved state, so saving only counts it
  save = function(canvas)
    canvas.unsaved = false
    saves = saves + 1
    canvas.panel:setUnsaved(false)
    ecs.log.info(canvas.panel:getTitle() .. " is saved.")
    return true
  end,
})

-- clock -----------------------------------------------------------------

-- a dark face with twelve marks and a hand that turns once a minute; a continuous panel, so it is drawn every frame
ecs.panel.registerType({
  name = name("clock"),
  title = "Lua clock",
  continuous = true,

  create = function(panel)
    return { panel = panel, time = 0, shownSeconds = -1 }
  end,

  draw = function(clock, surface, seconds)
    clock.time = clock.time + seconds
    local width, height = surface.width, surface.height
    local background = string.pack("=I4", BACKGROUND):rep(width)

    for y = 0, height - 1 do
      surface:setRow(y, background)
    end

    local cx, cy = width / 2, height / 2
    local radius = math.min(cx, cy) * 0.8

    -- the marks and the hand are dots along lines from the centre
    for mark = 0, 11 do
      local angle = mark * math.pi / 6
      local x, y = math.floor(cx + math.sin(angle) * radius), math.floor(cy - math.cos(angle) * radius)

      for dy = -2, 2 do
        for dx = -2, 2 do
          surface:setPixel(x + dx, y + dy, COLORS.white)
        end
      end
    end

    local angle = (clock.time % 60) / 60 * 2 * math.pi

    for t = 0, radius - 1 do
      surface:setPixel(math.floor(cx + math.sin(angle) * t), math.floor(cy - math.cos(angle) * t), CLOCK_HAND)
    end

    -- the title changes once a second, not every frame
    local whole = math.floor(clock.time)

    if whole ~= clock.shownSeconds then
      clock.panel:setTitle(("Lua clock %d:%02d"):format(whole // 60, whole % 60))
      clock.shownSeconds = whole
    end
  end,
})

-- export ----------------------------------------------------------------

-- paints a canvas into a PPM image; Lua never runs on worker threads, so this runs on the main thread
local function canvasPpm(canvas)
  local width, height = canvas.width, canvas.height
  local pixels = {}

  for i = 1, width * height do
    pixels[i] = BACKGROUND
  end

  paint(width, height, canvas.strokes, function(x, y, color)
    pixels[y * width + x + 1] = color
  end)

  local rows = { ("P6\n%d %d\n255\n"):format(width, height) }

  for y = 0, height - 1 do
    local bytes = {}

    for x = 1, width do
      local color = pixels[y * width + x]
      bytes[#bytes + 1] = (color >> 16) & 0xFF
      bytes[#bytes + 1] = (color >> 8) & 0xFF
      bytes[#bytes + 1] = color & 0xFF
    end

    rows[#rows + 1] = string.char(table.unpack(bytes))
  end

  return table.concat(rows)
end

local function exportChosen(id, files)
  local panel = ecs.layout.find(id)
  local canvas = panel and files and canvases[panel]

  if not panel or not canvas or canvas.width <= 0 then
    return
  end

  -- the canvas is painted and written at once; sketch_c does both on a worker thread
  local image = canvasPpm(canvas)
  ecs.log.debug(("Canvas %d is painted; writing %s."):format(id, files[1]))
  local file = io.open(files[1], "wb")
  local written = file and file:write(image) and file:close()

  if written then
    ecs.log.info(("%s was exported to %s."):format(panel:getTitle(), files[1]))
  else
    ecs.log.warn(("%s could not be exported to %s."):format(panel:getTitle(), files[1]))
  end
end

-- services --------------------------------------------------------------

local services = {}

function services.strokeCount(panel)
  local canvas = findCanvas(panel)
  return canvas and #canvas.strokes or 0
end

-- clears a canvas; if it has unsaved strokes, the user is asked first
function services.clear(panel)
  local canvas = findCanvas(panel)

  if not canvas or #canvas.strokes == 0 then
    return
  end

  -- without a dialog, the canvas is cleared
  if canvas.unsaved then
    local button = ecs.dialog.message("Clear canvas", "This canvas has unsaved strokes. Clear them?", { "Clear", "Keep" })

    if button == 2 then
      return
    end
  end

  canvas.strokes = {}
  canvasChanged(canvas)
end

function services.export(panel)
  if not findCanvas(panel) then
    return
  end

  local id = panel:getId()
  local shown = ecs.dialog.show(
    { type = "saveFile", filters = { { name = "PPM images", pattern = "ppm" } }, location = "canvas.ppm" },
    function(files)
      exportChosen(id, files)
    end)

  if not shown then
    ecs.log.warn("Cannot show the export dialog.")
  end
end

function services.copy(panel)
  local canvas = findCanvas(panel)

  if canvas and ecs.clipboard.setData(CLIPBOARD_TYPE, strokesToText(canvas.strokes)) then
    ecs.log.info(("Copied %d strokes."):format(#canvas.strokes))
  end
end

-- pastes strokes from the clipboard: strokes data if there is some, otherwise text in the same format
function services.paste(panel)
  local canvas = findCanvas(panel)

  if not canvas then
    return
  end

  local text = ecs.clipboard.getData(CLIPBOARD_TYPE) or ecs.clipboard.getText() or ""
  local added = strokesFromText(canvas, text)
  ecs.log.info(("Pasted %d strokes."):format(added))

  if added > 0 then
    canvasChanged(canvas)
  end
end

-- opens a canvas with the strokes of a file in the text format of copy; the function a preset opens files with
function services.open(path)
  local text = readFile(path)
  local panel = text and ecs.layout.open(name("canvas"))
  local canvas = panel and findCanvas(panel)

  if not canvas then
    ecs.log.warn(("Cannot open '%s'."):format(path))
    return
  end

  local added = strokesFromText(canvas, text)
  ecs.log.info(("Opened %d strokes from '%s'."):format(added, path))
  canvasTitle(canvas)
  canvas.panel:redraw()
end

function services.openBeside(panel)
  local opened = ecs.layout.open(name("canvas"), nil, panel, "right")

  if not opened then
    ecs.log.warn("Cannot open a canvas.")
  end
end

-- moves every other canvas into the group of a panel, and focuses the panel
function services.gather(panel)
  for other in pairs(canvases) do
    if other ~= panel and not ecs.layout.move(other, panel, "center") then
      ecs.log.warn("Cannot move " .. other:getTitle() .. ".")
    end
  end

  ecs.layout.focus(panel)
end

-- closes every other canvas; each may ask about unsaved work
function services.closeOthers(panel)
  local closed = 0

  -- a closed canvas is destroyed after this call, so the table does not change while it is walked
  for other in pairs(canvases) do
    if other ~= panel and ecs.layout.close(other) then
      closed = closed + 1
    end
  end

  return closed
end

function services.nextWorkspace()
  local next = ecs.workspace.getCurrent() % ecs.workspace.count() + 1
  ecs.workspace.switch(next)
  ecs.log.info(("Workspace %d, '%s'."):format(next, ecs.workspace.getName(next)))
end

-- calls a function with every open canvas and its number of strokes; the function is valid only during the call
function services.eachCanvas(fn)
  local count = 0

  for panel, canvas in pairs(canvases) do
    if fn then
      fn(panel, #canvas.strokes)
    end

    count = count + 1
  end

  return count
end

-- gives the number of strokes drawn in every session, and a table of numbers about the plugin
function services.stats()
  local own = 0

  -- the plugin's settings are counted from the list of every setting
  for _, setting in ipairs(ecs.settings.list() or {}) do
    if setting:sub(1, #NAME + 1) == NAME .. "." then
      own = own + 1
    end
  end

  local focus = ecs.layout.getFocus()
  return total,
      {
        canvases = openCanvases,
        strokes = total,
        saves = saves,
        settings = own,
        focus = focus and focus:getId() or 0,
        workspace =
            ecs.workspace.getCurrent()
      }
end

-- gives a canvas as a PPM image
function services.pixels(panel)
  local canvas = findCanvas(panel)
  return canvas and canvas.width > 0 and canvasPpm(canvas) or ""
end

-- makes a brush, which a canvas can use instead of the settings' brush
function services.brush(size, color)
  return ecs.handle.new(name("brush"), { size = clamp(size, MIN_SIZE, MAX_SIZE), color = COLORS[color] or COLORS.white })
end

-- the canvas keeps the brush's value, so the brush lives while the canvas uses it
function services.useBrush(brush, panel)
  local canvas = findCanvas(panel)

  if canvas then
    canvas.brush = ecs.handle.value(brush, name("brush"))
  end
end

ecs.handle.registerType(name("brush"))

assert(ecs.service.register(NAME, {
  strokeCount = { sig = "int(handle<ecs.panel>)", doc = "Counts a canvas's strokes", fn = services.strokeCount },
  clear = { sig = "void(handle<ecs.panel>)", doc = "Clear the canvas", fn = services.clear },
  export = { sig = "void(handle<ecs.panel>)", doc = "Export the canvas as an image", fn = services.export },
  copy = { sig = "void(handle<ecs.panel>)", doc = "Copy the canvas's strokes", fn = services.copy },
  paste = { sig = "void(handle<ecs.panel>)", doc = "Paste strokes", fn = services.paste },
  open = { sig = "void(string)", doc = "Open a canvas with the strokes of a file", fn = services.open },
  openBeside = { sig = "void(handle<ecs.panel>)", doc = "Open a canvas beside this one", fn = services.openBeside },
  gather = { sig = "void(handle<ecs.panel>)", doc = "Gather every canvas into this group", fn = services.gather },
  closeOthers = { sig = "int(handle<ecs.panel>)", doc = "Close the other canvases", fn = services.closeOthers },
  nextWorkspace = { sig = "void()", doc = "Switch to the next workspace", fn = services.nextWorkspace },
  stats = { sig = "int(out value)", doc = "Counts strokes, canvases and saves", fn = services.stats },
  eachCanvas = { sig = "int(fn<void(handle<ecs.panel>, int)>)", doc = "Calls a function with every canvas and its number of strokes", fn = services.eachCanvas },
  pixels = { sig = "buffer(handle<ecs.panel>)", doc = "Gives a canvas as a PPM image", fn = services.pixels },
  brush = { sig = "handle<sketch_lua.brush>(int, string)", doc = "Makes a brush of a size and a colour", fn = services.brush },
  useBrush = { sig = "void(handle<sketch_lua.brush>, handle<ecs.panel>)", doc = "Makes a canvas draw with a brush", fn = services.useBrush },
}))

-- default keys that work while a canvas has the focus; the canvas's menu shows the same functions with their keys
for key, service in pairs({ Delete = "clear", ["Ctrl+E"] = "export", ["Ctrl+C"] = "copy", ["Ctrl+V"] = "paste", ["Ctrl+B"] = "openBeside", ["Ctrl+G"] = "gather" }) do
  assert(ecs.input.bind(name("canvas"), key, name(service)))
  assert(ecs.panel.addMenuEntry(name("canvas"), name(service)))
end

-- events ----------------------------------------------------------------

ecs.event.declare(name("strokeAdded"), "A canvas has a new stroke: { panel = id, strokes = count }")

local function onEvent(event, value)
  if event == name("strokeAdded") then
    total = total + 1
  elseif event == "ecs.panelOpened" and value.type == name("canvas") then
    openCanvases = openCanvases + 1
  elseif event == "ecs.panelClosed" and value.type == name("canvas") then
    openCanvases = openCanvases - 1
  elseif event == "ecs.workspaceSwitched" then
    ecs.log.debug(("Workspace %d, '%s', is shown."):format(value.workspace, ecs.workspace.getName(value.workspace)))
  end
end

for _, event in ipairs({ name("strokeAdded"), "ecs.panelOpened", "ecs.panelClosed", "ecs.workspaceSwitched" }) do
  ecs.event.subscribe(event, onEvent)
end

ecs.plugin.registerState({
  version = STATE_VERSION,
  save = function()
    return { total = total, saves = saves }
  end,
  restore = function(state)
    total, saves = state and state.total or 0, state and state.saves or 0
    ecs.log.info(("Restored: %d strokes and %d saves so far."):format(total, saves))
  end,
})

-- logs the numbers of sketch_lua.stats now and then, and each canvas through sketch_lua.eachCanvas; a plugin timer
local eachCanvas = assert(ecs.service.get(name("eachCanvas"), "int(fn<void(handle<ecs.panel>, int)>)"))
local statsTimer = ecs.timer.start(60, true, function()
  local strokes, numbers = services.stats()
  ecs.log.debug(("%d strokes in all, %d canvases open."):format(strokes, numbers.canvases))
  eachCanvas(function(panel, count)
    ecs.log.debug(("%s has %d strokes."):format(panel:getTitle(), count))
  end)
end)

ecs.plugin.onShutdown(function()
  statsTimer:stop()
  ecs.log.info(("Goodbye after %d strokes."):format(total))
end)

-- a service is called through its lookup like any other plugin would call it
local stats = assert(ecs.service.get(name("stats"), "int(out value)"))
ecs.log.debug("brushSize comes from the " .. ecs.settings.explain(name("brushSize")).layer .. " layer.")
local strokes, numbers = stats()
ecs.log.info(("Ready with %d settings; %d strokes so far."):format(numbers.settings, strokes))
