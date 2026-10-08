-- Example Lua plugin: it uses the ecs table that the core gives it.

ecs.log.info("Hello from " .. ecs.plugin.name .. " " .. ecs.plugin.version .. ".")

ecs.settings.declare({
  name = "hello.greetings",
  type = "integer",
  description = "How many greetings the hello plugin logs",
  default = 2,
})

-- the plugin's own state: how many greetings it has logged in every session so far
local total = 0

ecs.plugin.register_state({
  version = 1,
  save = function() return { total = total } end,
  restore = function(state, version)
    total = state and state.total or 0
    ecs.log.info("Restored " .. total .. " greetings from the session.")
  end,
})

-- a repeating timer that stops itself after a few calls
local count = 0
local timer
timer = ecs.timer.start(0.5, true, function()
  count = count + 1
  total = total + 1
  ecs.log.info("Greeting " .. count .. ".")

  if count >= ecs.settings.get("hello.greetings") then
    timer:stop()
  end
end)

-- a panel type written in Lua: stripes whose colour changes on click, or with a key
local colors = { 0xFF3B4252, 0xFF88C0D0, 0xFFA3BE8C, 0xFFEBCB8B }
local stripes = setmetatable({}, { __mode = "k" }) -- each panel's state, by its handle

local function next_color(state)
  state.color = state.color % #colors + 1
  state.clicks = state.clicks + 1
  state.panel:set_title("Stripes " .. state.clicks)
  state.panel:redraw()
end

ecs.panel.register_type({
  name = "hello.stripes",
  title = "Stripes",
  state_version = 1,

  create = function(panel, saved, version)
    local color = saved and saved.color or 1
    local state = { panel = panel, color = colors[color] and color or 1, clicks = 0 }
    stripes[panel] = state
    return state
  end,

  draw = function(state, surface)
    local row = string.pack("=I4", colors[state.color]):rep(surface.width)
    local dark = string.pack("=I4", 0xFF2E3440):rep(surface.width)

    for y = 0, surface.height - 1 do
      surface:set_row(y, (y // 16) % 2 == 0 and row or dark)
    end
  end,

  event = function(state, event)
    if event.type == "pointer_down" then
      next_color(state)
    elseif event.type == "shown" or event.type == "resized" then
      ecs.log.debug(("Stripes %s at %dx%d."):format(event.type, event.width, event.height))
    elseif event.type == "hidden" then
      ecs.log.debug("Stripes hidden.")
    end
  end,

  save_state = function(state)
    return { color = state.color }
  end,
})

-- C functions of a service that the manifest depends on
local add = assert(ecs.service.get("demo.add", "int(int, int)"))
local repeat_text = assert(ecs.service.get("demo.repeat"))
ecs.log.info("demo.add(2, 3) = " .. add(2, 3) .. ", demo.repeat = " .. repeat_text("ab", 3))

-- a Lua function of a service; C plugins that depend on hello get it as a typed C function pointer
ecs.service.register("hello", {
  shout = {
    sig = "string(string, int)",
    doc = "Repeats a text in capitals",
    fn = function(text, count) return text:upper():rep(count, " ") end,
  },
})

-- output parameters are extra results; values and buffers are copied
local whole, fraction = ecs.service.get("demo.split")(3.25)
local type, counted = ecs.service.get("demo.describe")({ 1, 2, 3, name = "x" })
ecs.log.info(("demo.split: %d %.2f, demo.describe: %d %d, demo.reverse: %s"):format(whole, fraction, type, counted.items, ecs.service.get("demo.reverse")("abc")))

-- a handle stands for an object of the demo plugin; the garbage collector frees it
local counter = ecs.service.get("demo.counter")(10)
ecs.log.info("counter: " .. ecs.service.get("demo.counter_add")(counter, 5) .. " " .. tostring(counter))
counter = nil
collectgarbage()

-- a key for the stripes panel: its setting holds the key, so the user can change it
ecs.service.register("hello", {
  next_color = {
    sig = "void(handle<ecs.panel>)",
    doc = "Shows the next colour of a stripes panel",
    fn = function(panel) next_color(stripes[panel]) end,
  },
})
ecs.settings.declare({ name = "hello.next_color_key", type = "key", default = "N", description = "Key for the next colour of a stripes panel" })
ecs.input.bind("hello.stripes", "hello.next_color_key", "hello.next_color")

-- code opens panels too: a key opens another stripes panel beside the focused panel
ecs.service.register("hello", {
  open_stripes = {
    sig = "void()",
    doc = "Opens a stripes panel beside the focused panel",
    fn = function() ecs.layout.open("hello.stripes", { color = 3 }, ecs.layout.get_focus(), "right") end,
  },
})

-- named events: one of the core's, and one that the demo plugin declares; hello depends on demo, so it may hear it
ecs.event.subscribe("ecs.workspace_switched", function(name, value)
  ecs.log.info("Workspace " .. value.workspace .. " is shown.")
end)

ecs.event.subscribe("demo.color_changed", function(name, value)
  ecs.log.info("A demo color panel changed to colour " .. value .. ".")
end)
