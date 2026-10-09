-- typed text goes into the focused field, and Backspace removes a whole character
return {
  preset = "preset.lua",
  run = function(test)
    test.text("Hello, wörld")
    test.key("Backspace")
    test.key("Backspace")
    test.key("Backspace")
    test.text("rld!")

    test.match(test.session().workspaces[1].windows[1].panels[1].state, { text = "Hello, wörld!" })
  end,
}
