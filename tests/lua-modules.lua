-- a Lua plugin's require finds the modules in the plugin's folder and runs them once, in the plugin's environment

return {
  preset = "presets/modular.lua",
  run = function(test)
    test.match(test.session().pluginState.modular.state, {
      name = "modular",
      shapes = 3,
      shared = true,
      once = true,
      tools = true,
      global = true,
      broken = false,
      message = true,
      missing = false,
    }, "the modules")
  end,
}
