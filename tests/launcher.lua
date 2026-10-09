-- a test without a preset starts from the launcher, which lists the presets, then the saved sessions, and chooses one with keys or a click

-- the chosen entry, as the launcher's panel saves it
local function chosen(test)
  return test.session().workspaces[1].windows[1].panels[1].state.selected
end

return {
  presets = "files/presets",
  sessions = "files/sessions",
  run = function(test)
    -- the paint preset first, then the sessions; the launcher's own preset is not listed
    test.match(chosen(test), { kind = "preset", name = "paint" }, "the first entry")

    test.call("launcher.down")
    test.match(chosen(test), { kind = "session", name = "drawing" }, "after down")

    test.call("launcher.down")
    test.call("launcher.down")
    test.match(chosen(test), { kind = "session", name = "notes" }, "down stops at the last entry")

    test.call("launcher.up")
    test.match(chosen(test), { kind = "session", name = "drawing" }, "after up")

    -- the keys are bound for the list
    test.key("Down")
    test.match(chosen(test), { kind = "session", name = "notes" }, "after the Down key")

    -- a click chooses the entry under it and opens it; a test cannot open one, so the launcher stays
    local list = test.rect(1)
    test.click(list.x + 100, list.y + 90)
    test.match(chosen(test), { kind = "preset", name = "paint" }, "after a click on the first entry")

    test.call("launcher.open")
    test.match(chosen(test), { kind = "preset", name = "paint" }, "after open")
  end,
}
