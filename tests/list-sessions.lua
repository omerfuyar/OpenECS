-- ecs.session.presets and ecs.session.sessions list presets and saved sessions, and a test cannot open a preset

-- the entry of a list with a name
local function find(list, name)
  for _, entry in ipairs(list) do
    if entry.name == name then
      return entry
    end
  end
end

return {
  preset = "presets/lister.lua",
  presets = "files/presets",
  sessions = "files/sessions",
  run = function(test)
    test.call("lister.list")
    local results = test.session().pluginState.lister.state

    -- a test lists the first-party presets and its own presets folder, and finds no last session of their tools
    local paint = find(results.presets, "paint")
    test.match(paint, { appId = "org.example.Paint", appName = "Paint" }, "the paint preset")
    assert(paint.path:match("files/presets/paint%.lua$"), "the paint preset's path: " .. paint.path)
    assert(paint.lastUsed == nil, "a test has no last sessions")

    -- a preset with listed = false is not listed
    test.match(find(results.presets, "hidden"), nil, "the hidden preset")

    -- the sessions are sorted by name, and the file that cannot be read is left out
    test.match(results.sessions, {
      { name = "drawing", appId = "openecs.paint", appName = "OpenECS" },
      { name = "notes", appId = "org.example.Notes", appName = "Notes" },
    }, "the sessions")

    for _, session in ipairs(results.sessions) do
      assert(session.path:match("files/sessions/" .. session.name .. "%.lua$"), "a session's path: " .. session.path)
      assert(math.type(session.saved) == "integer" and session.saved > 0, "a session's time: " .. tostring(session.saved))
    end

    test.match(results.opened, false, "opening a preset during a test")
  end,
}
