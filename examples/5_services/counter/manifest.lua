---@type ecs.Manifest
return {
  name = "counter",
  version = "0.1.0",
  api = 1,
  description = "Named counters that other plugins add to, in C",
  native = "libcounter.so",
}
