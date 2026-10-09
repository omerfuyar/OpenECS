-- A preset describes a tool: the plugins it needs and where their panels go.
-- Run it with: OpenECS --preset examples/1_hello/preset.lua
return {
  format = 1,
  name = "hello",
  version = "0.1.0",
  app = { id = "openecs.example.hello", name = "Hello" }, -- the id names the tool's last session
  depends = { hello = "0.1" },                            -- the plugins to load, with the lowest version each accepts
  pluginsDir = ".",                                       -- plugins are looked for in this folder first
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "hello.greeting" } } } } },
  },
}
