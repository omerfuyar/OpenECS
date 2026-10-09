-- every tool loads the settings window, which the keys after the prefix open; it chooses settings and changes them in its layer

-- the settings window's panel in a session, if it is open
local function window(session)
  local function find(node)
    for _, panel in ipairs(node.panels or {}) do
      if panel.type == "settings.window" then
        return panel
      end
    end

    for _, child in ipairs(node) do
      local found = find(child)

      if found then
        return found
      end
    end
  end

  return find(session.workspaces[1].windows[1])
end

-- chooses a setting with the window's keys
local function choose(test, name)
  for _ = 1, 100 do
    local selected = window(test.session()).state.selected

    if selected == name then
      return
    end

    test.call(selected < name and "settings.down" or "settings.up")
  end

  error("cannot choose " .. name)
end

return {
  preset = "presets/probe.lua",
  run = function(test)
    test.key("Alt+W")
    test.key(",")
    assert(window(test.session()), "the settings window is open")

    -- the core's settings come first, by name
    test.match(window(test.session()).state.selected, "ecs.colorAccent", "the first setting")

    -- a choice cycles, an integer steps, and the window's keys are bound for it
    choose(test, "ecs.focus")
    test.key("Right")
    test.match(test.session().pluginState.probe.state.focus, "hover", "ecs.focus after Right")
    test.key("Right")
    test.match(test.session().pluginState.probe.state.focus, "click", "ecs.focus after a second Right")

    choose(test, "ecs.reopenLimit")
    test.call("settings.previous")
    test.match(test.session().pluginState.probe.state.reopenLimit, 19, "ecs.reopenLimit after one step back")

    -- opening it again shows the same window
    test.call("settings.open")
    test.match(window(test.session()).state.selected, "ecs.reopenLimit", "the open window, shown again")

    -- Return types a text setting's value: a key the panel gets, then the text it types, as an input method gives them
    local function type(text)
      test.key("A")
      test.text(text)
    end

    choose(test, "ecs.colorAccent")
    test.key("Return")

    for _ = 1, #"#4C8BF5" do
      test.key("Backspace")
    end

    type("#1122")

    -- the core's keys type nothing: the text of the prefix key is dropped
    test.key("Alt+W")
    test.text("w")
    test.key("Escape")

    type("33")
    test.key("Return")
    test.match(test.session().pluginState.probe.state.accent, "#112233", "ecs.colorAccent after typing it")

    -- Escape drops what was typed
    test.key("Return")
    type("ff")
    test.key("Escape")
    test.key("Return")
    test.key("Return")
    test.match(test.session().pluginState.probe.state.accent, "#112233", "ecs.colorAccent after Escape")

    -- a number is typed too, and must be one
    choose(test, "ecs.reopenLimit")
    test.key("Return")
    test.key("Backspace")
    test.key("Backspace")
    type("x")
    test.key("Return")
    test.match(test.session().pluginState.probe.state.reopenLimit, 19, "ecs.reopenLimit after typing a word")
    test.key("Backspace")
    type("7")
    test.key("Return")
    test.match(test.session().pluginState.probe.state.reopenLimit, 7, "ecs.reopenLimit after typing it")

    -- a list is typed as a Lua table
    choose(test, "probe.sizes")
    test.key("Return")

    for _ = 1, #"{ 1, 2 }" do
      test.key("Backspace")
    end

    type("{ 3, 4.5 }")
    test.key("Return")
    test.match(test.session().pluginState.probe.state.sizes, { 3, 4.5 }, "probe.sizes after typing it")
  end,
}
