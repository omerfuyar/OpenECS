---
name: measure
description: Measure the speed of OpenECS, a standard plugin or an example - a frame, a hover, a service call, a fill - in a Release build run offscreen, with LuaJIT's profiler and trace log. Use for any claim about speed, before and after an optimization, and in reviews that rank work by its gain.
---

# Measure speed

A claim about speed names its numbers, the build they come from, and how they were made. Measure Release: a Debug build's sanitizers make C several times slower and allocations slower still. Use Debug only for what a sanitizer must see.

## 1. Build Release with the examples

```sh
.github/scripts/build.sh R
```

Every build runs tests (DESIGN.md 17.5). A test runs offscreen, reads none of the user's files and writes no session, so it never opens a window on the owner's screen.

## 2. Write the measurement as a test

Put it in the scratchpad, never in the repositories: a `preset.lua` with the plugins and panels to measure, and a `test.lua` that drives them and times the work with `os.clock()`:

```lua
return {
  preset = "preset.lua",
  run = function(test)
    test.wait(0.5) -- let the first frames and the JIT settle
    local start = os.clock()
    for _ = 1, 200 do
      test.move(561, 635) -- two points over different elements, so each move redraws
      test.move(561, 682)
    end
    io.stderr:write(("400 hovers: %.1f ms\n"):format((os.clock() - start) * 1000))
  end,
}
```

- To time calls, write a small Lua plugin beside the preset (`pluginsDir = "."`) whose panel's `draw` runs a call many times, such as `draw.fill(surface, 0, 0, 1, 1, "#4C8BF5FF")` 200000 times, and prints the nanoseconds per call.
- Take a screenshot with `test.screenshot(path)` once, to check that the pointer is over what you think.
- Run each measurement three times, and give the middle one.

```sh
build/Release/bin/OpenECS --test <scratchpad>/bench/test.lua
```

## 3. Find where the time goes

- **jit.p**, LuaJIT's sampling profiler, around the timed part: `require("jit.p").start("F", file)`, then `require("jit.p").stop()`. Modes:
  - `F`: functions, with their files; `l`: lines.
  - `v`: the virtual machine's states. A large `Interpreted` share means traces abort; a large `Garbage Collector` share means too many allocations.
  - `vF`: each state split by function.
- **jit.v**, the trace log: `require("jit.v").start(file)`. Count the aborts with `grep -- '---' file`. The usual causes are `NYI: bytecode FNEW` (a closure made in the loop), `UCLO` (a closure that captures the loop's locals), `unsupported C type conversion`, and `loop unroll limit reached` (deep recursion).
- Many plugins have a file named `init.lua`, so jit.p and jit.v name them alike. Find the plugin by the line's content.
- C has no Lua samples: jit.p counts its time as `C code`, or under the Lua function that called it.
- The core's profile (DESIGN.md 3.5), through the profiler plugin (Alt+W, then F), gives each plugin's and panel's time in a running tool.

## 4. Compare

Measure before and after a change, with the same test. Give both numbers, the build and the machine's load in the finding, commit or pull request. A difference smaller than the spread of three runs is no difference.
