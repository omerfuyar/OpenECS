-- Tests of an example run in Debug builds: OpenECS --test examples/1_hello/test.lua
return {
  preset = "preset.lua", -- relative to this file
  run = function(test)
    -- the preset's panel has the first id; a panel whose plugin failed has a fault
    test.match(test.panel(1), { type = "hello.greeting", title = "Greeting" })
    assert(test.panel(1).fault == nil, test.panel(1).fault)
  end,
}
