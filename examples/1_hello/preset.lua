-- A preset describes a tool: the plugins it needs and where their panels go.
-- Run it with: OpenECS --preset examples/1_hello/preset.lua
-- ---@type tells editors the file's shape (include/ecs.lua), so they complete and check its fields.
---@type ecs.Preset
return {
  format = 1,         -- the version of the file format, which changes when older OpenECS versions cannot read new files
  name = "hello",     -- the name the launcher and --preset know the preset by
  version = "0.1.0",  -- the preset's own version: major.minor.patch
  app = { id = "openecs.example.hello", name = "Hello" }, -- the id names the tool's last session, the name is the window's title
  depends = { hello = "0.1" },                            -- the plugins to load, with the lowest version each accepts
  pluginsDir = ".",                                       -- plugins are looked for in this folder first
  -- a workspace is a named arrangement of panels; each window holds a layout tree, here one group with one panel
  workspaces = {
    { name = "Main", windows = { { panels = { { type = "hello.greeting" } } } } },
  },
}
