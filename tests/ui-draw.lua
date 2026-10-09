-- the ui standard plugin draws rectangles, text, images and elements into another plugin's panel, and reads colours

return {
  preset = "presets/painter.lua",
  run = function(test)
    test.call("painter.useKept")
    test.call("painter.readColors")
    local results = test.session().pluginState.painter.state

    -- an opaque fill replaces the pixels, a translucent one blends, and a rectangle outside the surface is clipped
    test.match(results.red, 0xFFFF0000, "an opaque fill")
    test.match(results.blended, 0xFF808080, "a fill at half alpha over black")
    test.match(results.corner, 0xFF000000, "a fill outside the surface")

    -- text has a width, the same as measure gives, and draws only where it is
    assert(results.width > 0, "the text's width: " .. tostring(results.width))
    test.match(results.measured, true, "measure gives the drawn width and a line height")
    test.match(results.inked, true, "the text's pixels")
    test.match(results.outside, 0, "pixels beside the text")

    -- the other shapes, the plugin's image and the elements
    test.match(results.edge, 0xFFFFFFFF, "an outline's edge")
    test.match(results.middle, 0xFF000000, "inside an outline")
    test.match(results.imageDrawn, true, "an image is drawn")
    test.match(results.green, 0xFF00FF00, "an image's pixels")
    test.match(results.missing, false, "a missing image")
    test.match(results.size, { true, 4.0, 4.0 }, "an image's size")
    test.match(results.checked, true, "a check box that is on")
    test.match(results.cursor, true, "a field's cursor")
    test.match(results.thumb, true, "a scrollbar's thumb and track")
    test.match(results.thumbPlace, { 75.0, 25.0 }, "a thumb at the end")

    test.match(results.kept, false, "a surface handle after draw")

    local r, g, b = results.background:match("#(%x%x)(%x%x)(%x%x)")
    local background = 0xFF000000 | tonumber(r, 16) << 16 | tonumber(g, 16) << 8 | tonumber(b, 16)
    test.match(results.colors, { 0xFF102030, 0x80102030, background, 0xFFFF00FF }, "colours")
  end,
}
