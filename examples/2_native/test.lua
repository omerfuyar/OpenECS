return {
  preset = "preset.lua",
  run = function(test)
    test.match(test.panel(1), { type = "hello_c.greeting", title = "Greeting in C" })
    assert(test.panel(1).fault == nil, test.panel(1).fault)
  end,
}
