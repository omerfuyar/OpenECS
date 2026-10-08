-- Sketch in Lua: canvases to draw on with the mouse, and a clock. The sketch_c plugin does the same in C, so the two can be compared.

local NAME = ecs.plugin.name

-- makes a name of this plugin, such as "sketch_lua.canvas"
local function name(local_name)
  return NAME .. "." .. local_name
end

-- colours a brush can have, in ARGB8888, and their names in the setting sketch_lua.brush_color
local COLOR_NAMES = { "white", "red", "green", "blue", "yellow" }
local COLORS = { white = 0xFFECEFF4, red = 0xFFBF616A, green = 0xFFA3BE8C, blue = 0xFF5E81AC, yellow = 0xFFEBCB8B }
local BACKGROUND = 0xFF2E3440
local CLOCK_HAND = 0xFF88C0D0

-- the clipboard type of strokes; both sketch plugins use it, so strokes copied in one paste into the other
local CLIPBOARD_TYPE = "application/x-openecs-strokes"

-- version of the saved state of a canvas, and of the plugin's own state
local STATE_VERSION = 1

local MIN_SIZE, MAX_SIZE = 1, 64

local canvases = {} -- every open canvas's state, by its panel handle
local open_canvases = 0 -- counted from the core's events
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
local function strokes_to_text(strokes)
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

-- reads strokes written by strokes_to_text, and adds them to a canvas; gives how many it read
local function strokes_from_text(canvas, text)
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

-- canvas ----------------------------------------------------------------

local function find_canvas(panel)
  local canvas = canvases[panel]

  if not canvas then
    ecs.log.warn("That panel is not a sketch_lua canvas.")
  end

  return canvas
end

local function setting_size()
  return clamp(ecs.settings.get(name("brush_size")) or 6, MIN_SIZE, MAX_SIZE)
end

local function setting_color()
  return COLORS[ecs.settings.get(name("brush_color"))] or COLORS.white
end

-- shows the number of strokes in the canvas's title
local function canvas_title(canvas)
  canvas.panel:set_title(("Lua canvas (%d)"):format(#canvas.strokes))
end

-- marks a canvas changed: unsaved, retitled and redrawn
local function canvas_changed(canvas)
  canvas.unsaved = true
  canvas.panel:set_unsaved(true)
  canvas_title(canvas)
  canvas.panel:redraw()
end

-- tells subscribers that a canvas has a new stroke
local function emit_stroke(canvas)
  ecs.event.emit(name("stroke_added"), { panel = canvas.panel:get_id(), strokes = #canvas.strokes })
end

-- redraws every canvas when a brush setting changes
local function brush_changed()
  for _, canvas in pairs(canvases) do
    canvas.panel:redraw()
  end

  ecs.log.debug(("The brush is %d, %s."):format(setting_size(), ecs.settings.get(name("brush_color"))))
end

ecs.settings.declare({ name = name("brush_size"), type = "integer", description = "Size of the brush, in pixels; the wheel over a canvas changes it", default = 6, changed = brush_changed })
ecs.settings.declare({ name = name("brush_color"), type = "choice", description = "Colour of the brush", default = "white", choices = COLOR_NAMES, changed = brush_changed })
ecs.settings.declare({ name = name("reminder_seconds"), type = "number", description = "How often a canvas with unsaved strokes says so", default = 30.0 })
ecs.settings.declare({ name = name("clear_key"), type = "key", description = "Clears the canvas", default = "Delete" })
ecs.settings.declare({ name = name("export_key"), type = "key", description = "Exports the canvas as an image", default = "Ctrl+E" })
ecs.settings.declare({ name = name("copy_key"), type = "key", description = "Copies the canvas's strokes", default = "Ctrl+C" })
ecs.settings.declare({ name = name("paste_key"), type = "key", description = "Pastes strokes", default = "Ctrl+V" })
ecs.settings.declare({ name = name("beside_key"), type = "key", description = "Opens a canvas beside this one", default = "Ctrl+B" })
ecs.settings.declare({ name = name("gather_key"), type = "key", description = "Gathers every canvas into this group", default = "Ctrl+G" })

ecs.panel.register_type({
  name = name("canvas"),
  title = "Lua canvas",
  state_version = STATE_VERSION,
  min_width = 64,
  min_height = 64,

  create = function(panel, saved, version)
    local canvas = { panel = panel, strokes = {}, drawing = false, unsaved = false, width = 0, height = 0 }

    -- strokes = { { size = 6, color = 0xFFECEFF4, points = { x, y, ... } }, ... }; older versions are not read
    if version == STATE_VERSION and saved and saved.strokes then
      for _, stroke in ipairs(saved.strokes) do
        table.insert(canvas.strokes, { size = stroke.size or 6, color = stroke.color or COLORS.white, points = stroke.points or {} })
      end
    end

    canvases[panel] = canvas
    canvas_title(canvas)

    -- a panel timer, so it stops when the canvas closes
    panel:start_timer(math.max(1, ecs.settings.get(name("reminder_seconds")) or 30), true, function()
      if canvas.unsaved then
        ecs.log.info(panel:get_title() .. " has unsaved strokes.")
      end
    end)

    return canvas
  end,

  destroy = function(canvas)
    canvases[canvas.panel] = nil
  end,

  draw = function(canvas, surface)
    canvas.width, canvas.height = surface.width, surface.height
    local background = string.pack("=I4", BACKGROUND):rep(surface.width)

    for y = 0, surface.height - 1 do
      surface:set_row(y, background)
    end

    paint(surface.width, surface.height, canvas.strokes, function(x, y, color)
      surface:set_pixel(x, y, color)
    end)
  end,

  event = function(canvas, event)
    local panel = canvas.panel

    if event.type == "pointer_down" and event.button == 1 then
      local brush = canvas.brush
      table.insert(canvas.strokes, { size = brush and brush.size or setting_size(), color = brush and brush.color or setting_color(), points = { event.x, event.y } })
      canvas.drawing = true
      panel:redraw()
    elseif event.type == "pointer_move" and canvas.drawing then
      local points = canvas.strokes[#canvas.strokes].points
      points[#points + 1] = event.x
      points[#points + 1] = event.y
      panel:redraw()
    elseif event.type == "pointer_up" and canvas.drawing then
      canvas.drawing = false
      canvas_changed(canvas)
      emit_stroke(canvas)
    elseif event.type == "wheel" then
      -- the wheel changes the setting, so every canvas and the settings window see the new size
      local ok = ecs.settings.set(name("brush_size"), clamp(setting_size() + (event.wheel_y > 0 and 1 or -1), MIN_SIZE, MAX_SIZE))

      if not ok then
        ecs.log.warn("Cannot change the brush size.")
      end
    elseif event.type == "focused" or event.type == "unfocused" then
      ecs.log.debug(("%s is %s."):format(panel:get_title(), event.type))
    elseif event.type == "shown" or event.type == "resized" then
      ecs.log.debug(("%s is %s at %.0fx%.0f."):format(panel:get_title(), event.type, event.width, event.height))
    elseif event.type == "hidden" then
      ecs.log.debug(panel:get_title() .. " is hidden.")
    end
  end,

  save_state = function(canvas)
    return { strokes = canvas.strokes }
  end,

  -- saves a canvas's unsaved work; its strokes are already in its saved state, so saving only counts it
  save = function(canvas)
    canvas.unsaved = false
    saves = saves + 1
    canvas.panel:set_unsaved(false)
    ecs.log.info(canvas.panel:get_title() .. " is saved.")
    return true
  end,
})

-- clock -----------------------------------------------------------------

-- a dark face with twelve marks and a hand that turns once a minute; a continuous panel, so it is drawn every frame
ecs.panel.register_type({
  name = name("clock"),
  title = "Lua clock",
  continuous = true,

  create = function(panel)
    return { panel = panel, time = 0, shown_seconds = -1 }
  end,

  draw = function(clock, surface, seconds)
    clock.time = clock.time + seconds
    local width, height = surface.width, surface.height
    local background = string.pack("=I4", BACKGROUND):rep(width)

    for y = 0, height - 1 do
      surface:set_row(y, background)
    end

    local cx, cy = width / 2, height / 2
    local radius = math.min(cx, cy) * 0.8

    -- the marks and the hand are dots along lines from the centre
    for mark = 0, 11 do
      local angle = mark * math.pi / 6
      local x, y = math.floor(cx + math.sin(angle) * radius), math.floor(cy - math.cos(angle) * radius)

      for dy = -2, 2 do
        for dx = -2, 2 do
          surface:set_pixel(x + dx, y + dy, COLORS.white)
        end
      end
    end

    local angle = (clock.time % 60) / 60 * 2 * math.pi

    for t = 0, radius - 1 do
      surface:set_pixel(math.floor(cx + math.sin(angle) * t), math.floor(cy - math.cos(angle) * t), CLOCK_HAND)
    end

    -- the title changes once a second, not every frame
    local whole = math.floor(clock.time)

    if whole ~= clock.shown_seconds then
      clock.panel:set_title(("Lua clock %d:%02d"):format(whole // 60, whole % 60))
      clock.shown_seconds = whole
    end
  end,
})

-- export ----------------------------------------------------------------

-- paints a canvas into a PPM image; Lua never runs on worker threads, so this runs on the main thread
local function canvas_ppm(canvas)
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

local function export_chosen(id, files)
  local panel = ecs.layout.find(id)
  local canvas = panel and files and canvases[panel]

  if not canvas or canvas.width <= 0 then
    return
  end

  -- the canvas is painted and written at once; sketch_c does both on a worker thread
  local image = canvas_ppm(canvas)
  ecs.log.debug(("Canvas %d is painted; writing %s."):format(id, files[1]))
  local file = io.open(files[1], "wb")
  local written = file and file:write(image) and file:close()

  if written then
    ecs.log.info(("%s was exported to %s."):format(panel:get_title(), files[1]))
  else
    ecs.log.warn(("%s could not be exported to %s."):format(panel:get_title(), files[1]))
  end
end

-- services --------------------------------------------------------------

local services = {}

function services.stroke_count(panel)
  local canvas = find_canvas(panel)
  return canvas and #canvas.strokes or 0
end

-- clears a canvas; if it has unsaved strokes, the user is asked first
function services.clear(panel)
  local canvas = find_canvas(panel)

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
  canvas_changed(canvas)
end

function services.export(panel)
  if not find_canvas(panel) then
    return
  end

  local id = panel:get_id()
  local shown = ecs.dialog.show({ type = "save_file", filters = { { name = "PPM images", pattern = "ppm" } }, location = "canvas.ppm" }, function(files)
    export_chosen(id, files)
  end)

  if not shown then
    ecs.log.warn("Cannot show the export dialog.")
  end
end

function services.copy(panel)
  local canvas = find_canvas(panel)

  if canvas and ecs.clipboard.set_data(CLIPBOARD_TYPE, strokes_to_text(canvas.strokes)) then
    ecs.log.info(("Copied %d strokes."):format(#canvas.strokes))
  end
end

-- pastes strokes from the clipboard: strokes data if there is some, otherwise text in the same format
function services.paste(panel)
  local canvas = find_canvas(panel)

  if not canvas then
    return
  end

  local text = ecs.clipboard.get_data(CLIPBOARD_TYPE) or ecs.clipboard.get_text() or ""
  local added = strokes_from_text(canvas, text)
  ecs.log.info(("Pasted %d strokes."):format(added))

  if added > 0 then
    canvas_changed(canvas)
  end
end

function services.open_beside(panel)
  local opened = ecs.layout.open(name("canvas"), nil, panel, "right")

  if not opened then
    ecs.log.warn("Cannot open a canvas.")
  end
end

-- moves every other canvas into the group of a panel, and focuses the panel
function services.gather(panel)
  for other in pairs(canvases) do
    if other ~= panel and not ecs.layout.move(other, panel, "center") then
      ecs.log.warn("Cannot move " .. other:get_title() .. ".")
    end
  end

  ecs.layout.focus(panel)
end

-- closes every other canvas; each may ask about unsaved work
function services.close_others(panel)
  local closed = 0

  -- a closed canvas is destroyed after this call, so the table does not change while it is walked
  for other in pairs(canvases) do
    if other ~= panel and ecs.layout.close(other) then
      closed = closed + 1
    end
  end

  return closed
end

function services.next_workspace()
  local next = ecs.workspace.get_current() % ecs.workspace.count() + 1
  ecs.workspace.switch(next)
  ecs.log.info(("Workspace %d, '%s'."):format(next, ecs.workspace.get_name(next)))
end

-- gives the number of strokes drawn in every session, and a table of numbers about the plugin
function services.stats()
  local own = 0

  -- the plugin's settings are counted from the list of every setting
  for _, setting in ipairs(ecs.settings.list()) do
    if setting:sub(1, #NAME + 1) == NAME .. "." then
      own = own + 1
    end
  end

  local focus = ecs.layout.get_focus()
  return total, { canvases = open_canvases, strokes = total, saves = saves, settings = own, focus = focus and focus:get_id() or 0, workspace = ecs.workspace.get_current() }
end

-- gives a canvas as a PPM image
function services.pixels(panel)
  local canvas = find_canvas(panel)
  return canvas and canvas.width > 0 and canvas_ppm(canvas) or ""
end

-- makes a brush, which a canvas can use instead of the settings' brush
function services.brush(size, color)
  return ecs.handle.new(name("brush"), { size = clamp(size, MIN_SIZE, MAX_SIZE), color = COLORS[color] or COLORS.white })
end

-- the canvas keeps the brush's value, so the brush lives while the canvas uses it
function services.use_brush(brush, panel)
  local canvas = find_canvas(panel)

  if canvas then
    canvas.brush = ecs.handle.value(brush, name("brush"))
  end
end

ecs.handle.register_type(name("brush"))

assert(ecs.service.register(NAME, {
  stroke_count = { sig = "int(handle<ecs.panel>)", doc = "Counts a canvas's strokes", fn = services.stroke_count },
  clear = { sig = "void(handle<ecs.panel>)", doc = "Clear the canvas", fn = services.clear },
  export = { sig = "void(handle<ecs.panel>)", doc = "Export the canvas as an image", fn = services.export },
  copy = { sig = "void(handle<ecs.panel>)", doc = "Copy the canvas's strokes", fn = services.copy },
  paste = { sig = "void(handle<ecs.panel>)", doc = "Paste strokes", fn = services.paste },
  open_beside = { sig = "void(handle<ecs.panel>)", doc = "Open a canvas beside this one", fn = services.open_beside },
  gather = { sig = "void(handle<ecs.panel>)", doc = "Gather every canvas into this group", fn = services.gather },
  close_others = { sig = "int(handle<ecs.panel>)", doc = "Close the other canvases", fn = services.close_others },
  next_workspace = { sig = "void()", doc = "Switch to the next workspace", fn = services.next_workspace },
  stats = { sig = "int(out value)", doc = "Counts strokes, canvases and saves", fn = services.stats },
  pixels = { sig = "buffer(handle<ecs.panel>)", doc = "Gives a canvas as a PPM image", fn = services.pixels },
  brush = { sig = "handle<sketch_lua.brush>(int, string)", doc = "Makes a brush of a size and a colour", fn = services.brush },
  use_brush = { sig = "void(handle<sketch_lua.brush>, handle<ecs.panel>)", doc = "Makes a canvas draw with a brush", fn = services.use_brush },
}))

-- keys work while a canvas has the focus, and the canvas's menu shows the same functions with their keys; the user changes the keys in the settings
for setting, service in pairs({ clear_key = "clear", export_key = "export", copy_key = "copy", paste_key = "paste", beside_key = "open_beside", gather_key = "gather" }) do
  assert(ecs.input.bind(name("canvas"), name(setting), name(service)))
  assert(ecs.panel.add_menu_entry(name("canvas"), name(service)))
end

-- events ----------------------------------------------------------------

ecs.event.declare(name("stroke_added"), "A canvas has a new stroke: { panel = id, strokes = count }")

local function on_event(event, value)
  if event == name("stroke_added") then
    total = total + 1
  elseif event == "ecs.panel_opened" and value.type == name("canvas") then
    open_canvases = open_canvases + 1
  elseif event == "ecs.panel_closed" and value.type == name("canvas") then
    open_canvases = open_canvases - 1
  elseif event == "ecs.workspace_switched" then
    ecs.log.debug(("Workspace %d, '%s', is shown."):format(value.workspace, ecs.workspace.get_name(value.workspace)))
  end
end

for _, event in ipairs({ name("stroke_added"), "ecs.panel_opened", "ecs.panel_closed", "ecs.workspace_switched" }) do
  ecs.event.subscribe(event, on_event)
end

ecs.plugin.register_state({
  version = STATE_VERSION,
  save = function()
    return { total = total, saves = saves }
  end,
  restore = function(state)
    total, saves = state and state.total or 0, state and state.saves or 0
    ecs.log.info(("Restored: %d strokes and %d saves so far."):format(total, saves))
  end,
})

-- logs the numbers of sketch_lua.stats now and then; a plugin timer
local stats_timer = ecs.timer.start(60, true, function()
  local strokes, numbers = services.stats()
  ecs.log.debug(("%d strokes in all, %d canvases open."):format(strokes, numbers.canvases))
end)

ecs.plugin.on_shutdown(function()
  stats_timer:stop()
  ecs.log.info(("Goodbye after %d strokes."):format(total))
end)

-- a service is called through its lookup like any other plugin would call it
local stats = assert(ecs.service.get(name("stats"), "int(out value)"))
ecs.log.debug("brush_size comes from the " .. ecs.settings.explain(name("brush_size")).layer .. " layer.")
local strokes, numbers = stats()
ecs.log.info(("Ready with %d settings; %d strokes so far."):format(numbers.settings, strokes))
