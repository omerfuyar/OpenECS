-- a colour dragged from the palette colours the sample, and files dropped on it show their names
return {
  preset = "preset.lua",
  run = function(test)
    local palette, sample = test.rect(1), test.rect(2)

    -- the third swatch, dragged onto the sample
    test.drag(palette.x + 2 * 48 + 10, palette.y + 10, sample.x + 50, sample.y + 50)
    test.dropFiles(sample.x + 50, sample.y + 50, { "/home/someone/notes.txt", "/home/someone/photo.png" })

    local state = test.session().workspaces[1].windows[1][2].panels[1].state
    test.match(state, { color = 0xFFEBCB8B, files = { "notes.txt", "photo.png" } })
  end,
}
