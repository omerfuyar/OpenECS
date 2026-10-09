-- The same plugin as 1_hello, in C. The build compiles every C file of the folder into the plugin's library.
---@type ecs.Manifest
return {
  name = "hello_c",
  version = "0.1.0",
  api = 1,
  description = "The smallest native plugin: one panel of one colour",
  native = "libhello_c.so", -- the library the build makes from the folder's C files
}
