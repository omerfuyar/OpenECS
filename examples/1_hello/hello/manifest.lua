-- A manifest describes a plugin. It is read as data, before any of the plugin's code runs.
---@type ecs.Manifest
return {
  name = "hello",     -- the folder's name; everything the plugin registers starts with it
  version = "0.1.0",  -- presets and other plugins ask for a lowest version with the same major number
  api = 1,            -- the plugin API version it was written for; OpenECS refuses other versions
  description = "The smallest plugin: one panel of one colour",
  lua = "init.lua",   -- the file that runs when the plugin loads
}
