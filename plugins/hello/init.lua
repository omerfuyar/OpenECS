-- Example Lua plugin: it uses the ecs table that the core gives it.

ecs.log.info("Hello from " .. ecs.plugin.name .. " " .. ecs.plugin.version .. ".")

ecs.settings.declare({
  name = "hello.greetings",
  type = "integer",
  description = "How many greetings the hello plugin logs",
  default = 2,
})

-- a repeating timer that stops itself after a few calls
local count = 0
local timer
timer = ecs.timer.start(0.5, true, function()
  count = count + 1
  ecs.log.info("Greeting " .. count .. ".")

  if count >= ecs.settings.get("hello.greetings") then
    timer:stop()
  end
end)

-- a panel type written in Lua: stripes whose colour changes on click
local colors = { 0xFF3B4252, 0xFF88C0D0, 0xFFA3BE8C, 0xFFEBCB8B }

ecs.panel.register_type({
  name = "hello.stripes",
  title = "Stripes",
  state_version = 1,

  create = function(panel, saved, version)
    local color = saved and saved.color or 1
    return { panel = panel, color = colors[color] and color or 1, clicks = 0 }
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
      state.color = state.color % #colors + 1
      state.clicks = state.clicks + 1
      state.panel:set_title("Stripes " .. state.clicks)
      state.panel:redraw()
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
