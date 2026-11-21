# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **SDL's tray closes the D-Bus connection of notifications.** On Linux, SDL's D-Bus tray calls `SDL_DBus_Quit` when its last tray goes, which closes the connection that SDL's notifications share: their clicks stop coming after notify's last tray goes. At exit, `SDL_CleanupNotifications` then removes a filter from a new connection, which libdbus aborts on; the core makes that a warning for its quit (`ECSIApp_QuitSDL`), so the workaround can go once SDL is fixed. It is SDL's bug, still in its `main`, for the owner to report: SDL takes no contributions written by AI.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **The lag on Windows.** The owner saw the editor lag on Windows in a Release build, on a fast machine (i7-14700, 64 GB) with low CPU and GPU use: on hovering, moving and resizing, and more in a larger window. It could not be reproduced on Linux, where software Vulkan hides it. Measure it with the profiler (Alt+W, then F; std DESIGN 32), and send its report; suspects are Direct3D 12's present and frame pacing, and `SDL_UpdateTexture` of large surfaces, which the steps `present` and `upload` show.
- **LuaJIT in place of Lua 5.5.** The owner decided it: OpenECS moves to LuaJIT 2.1. Measured in a Release build on Linux, with settings, keys, files and console redrawn whole 50 times a second, their Lua code took about 330 ms a second, draw's C 160 ms, and the calls between them about 3% of the Lua time (0.15 µs a call into C, against 0.03 µs into Lua; about a thousand calls a frame). The time is in the Lua code, mostly ui building and arranging its tree, which LuaJIT runs faster, so the bridge stays the C API: no bridge through the FFI. The work is one interface change (the interface-change skill), on `feature/luajit` in each repository, merged in the order of the pull-requests skill. Its steps:
  1. **The dependency.** Add `dependencies/luajit`, from https://github.com/LuaJIT/LuaJIT, branch `v2.1`, which LuaJIT releases from directly. Add `dependencies/lua-compat-5.3`, from https://github.com/lunarmodules/lua-compat-5.3 (MIT), if the owner agrees (question 1 below). Remove `dependencies/lua`. Update DESIGN 17.1 and 17.4, README.md and the licences the release ships.
  2. **The build.** `Shuild_luajit` in `shuild.c` replaces `Shuild_lua`. It does what LuaJIT's `src/Makefile` does, and writes every generated file under the build's folder, never into `dependencies/`:
     1. Build the host tool `minilua` from `src/host/minilua.c`, with the host's compiler and `-lm`.
     2. Run `minilua dynasm/dynasm.lua FLAGS -o buildvm_arch.h src/vm_x64.dasc`. The flags are `DASM_AFLAGS` of the Makefile (lines 406 to 488), which it reads from the target's `lj_arch.h` with `cc -E -dM`: on x64 `-D ENDIAN_LE -D P64 -D JIT -D FFI -D FPU -D HFABI -D VER=`, and `-D WIN` for Windows. Read them the same way, not as a fixed list, so aarch64 works later.
     3. Write `luajit.h` from `src/luajit_rolling.h` with `minilua src/host/genversion.lua`, which takes the release number from `luajit_relver.txt`: the time of the submodule's commit (`git show -s --format=%ct`), or `.relver`.
     4. Build the host tool `buildvm` from `src/host/buildvm*.c`, with the folder of `buildvm_arch.h` on its include path. For a cross build, the host tools get the target's `-DLUAJIT_TARGET` and `-DLUAJIT_OS`, as the Makefile's `HOST_XCFLAGS` does.
     5. Run `buildvm`: `-m elfasm -o lj_vm.S` on Linux, `-m peobj -o lj_vm.o` on Windows; `-m bcdef`, `-m ffdef`, `-m libdef` and `-m recdef` over the `LJLIB_C` files (Makefile, line 512), and `-m folddef` over `lj_opt_fold.c`, each to its `lj_*def.h`. `jit/vmdef.lua` is not needed: it serves `jit.v` and `jit.dump`, which OpenECS does not ship.
     6. Build the static library `luajit` from `src/ljamalg.c` and `lj_vm.S` or `lj_vm.o`, with `-DLUAJIT_ENABLE_LUA52COMPAT` and the generated folder first on the include path. Copy `lua.h`, `lauxlib.h`, `lualib.h`, `luaconf.h`, `luajit.h` and `lua.hpp` to `<dependencies>/include/lua/`, so the core's includes stay.
     7. Guard it with `IsBuilt("luajit", false)`, link `luajit` in place of `lua`, and build Debug and Release on Linux and with MinGW for Windows.
  3. **The C API.** The core includes lua-compat-5.3's `c-api/compat-5.3.h` after `lua.h`, through one header of its own. It gives the 5.3 forms the core uses: `lua_getfield`, `lua_rawgeti`, `lua_rawget` and `lua_getglobal` that return the type (13 places test it), `lua_rawlen`, `lua_absindex`, `lua_rawgetp`, `lua_rawsetp`, `luaL_getsubtable`, `luaL_tolstring`, `lua_isinteger` and `lua_pushglobaltable`. The rest is changed by hand, in `src/base/Lua.c`, `src/runtime/Services.c`, `src/app/Bindings.c`, `src/app/Workers.c` and `src/app/Test.c`:
     - `lua_newuserdatauv(state, size, 0)`, 11 calls, all without user values, becomes `lua_newuserdata(state, size)`.
     - `lua_newstate(allocate, nullptr, luaL_makeseed(nullptr))` becomes `lua_newstate(allocate, nullptr)`: LuaJIT seeds itself, and takes an allocator on x64, as GC64 is its default there.
     - `lua_setwarnf` and `ECSILua_Warn` go: LuaJIT has no `warn`. Plugins warn with `ecs.log.warn`.
     - `lua_resume(thread, from, count, &results)` (`Test.c`) becomes `lua_resume(thread, count)`, and the results are `lua_gettop(thread)`.
     - `lua_getextraspace` (`Workers.c`) goes: the worker is found through the registry, under a light userdata key of its state.
     - `luaL_typeerror` and `luaL_argexpected` become `luaL_typerror` and `luaL_argcheck` with the same text; `LUA_LOADED_TABLE` becomes `"_LOADED"`.
     - Errors: on x64 LuaJIT raises errors with the system's unwinder, not `longjmp`, so every C frame between a `lua_pcall` and a `lua_error` needs unwind tables. GCC and Clang emit them by default on x64, and libffi's assembly has them. Check that an error in a Lua callback that C calls through a service reaches its protected call (DESIGN 11.1), in Debug with the sanitizers too.
     - `luaL_openlibs` opens `bit`, `jit` and `ffi` as well; plugins have them, as they have `io` and `os` (DESIGN 9). Data files' environment (`src/base/Lua.c`, line 71) gets none of them.
  4. **Numbers.** LuaJIT has only doubles, exact as integers up to 2^53. A number is an integer when it is whole: lua-compat-5.3's `lua_isinteger` tests that. Settings of type `integer`, `int` and `i64` parameters, and `ECSValue` (DESIGN 10.3) read a whole number as an integer. The core's writer (`OPENECS_LUA_WRITER`, `src/base/Lua.c`, line 34) tests `n == math.floor(n)` in place of `math.type`. Write the rule into DESIGN 10.3 and 11.
  5. **The libraries plugins lose.** DESIGN 9 promises `utf8`; plugins also use `string.pack`, `string.unpack` and `string.packsize`. The core builds lua-compat-5.3's `lutf8lib.c` as `utf8`, and its `lstrlib.c` for `pack`, `unpack` and `packsize` only, which it adds to `string`. `table.pack`, `table.unpack`, `table.move`, `goto`, `load` with an environment and `__pairs`, `__ipairs` and `__len` on tables come with LuaJIT and `LUAJIT_ENABLE_LUA52COMPAT`.
  6. **The Lua code**, in std, examples, the template and `include/ecs.lua`. Find each with grep over `*.lua`:
     - `a // b` becomes `math.floor(a / b)`: std's ui, and c12_sketch, c13_workers, s1_terminal, s4_model, s9_gpu_model and s20_syntax.
     - Bitwise operators become the `bit` library (`bit.band`, `bit.bor`, `bit.bxor`, `bit.lshift`, `bit.rshift`): ui's colours, c12_sketch, s6_web's SHA-1, s8_gpu and s10_cglm. `bit` works on signed 32-bit numbers: a colour above `0x7FFFFFFF` comes out negative, so add `% 2^32` where a result must stay positive. LuaJIT 2.1 also parses `&`, `|`, `~`, `<<` and `>>`, but LuaLS reports them as errors for LuaJIT, so the code uses `bit`.
     - `math.type(n) == "integer"` becomes `n == math.floor(n)`, and `math.tointeger` likewise: std's settings, console and ui.
     - `goto` (std, once), `table.pack` and `table.unpack` stay.
  7. **Editors.** Each repository's `.luarc.json` sets `"runtime.version": "LuaJIT"`. `include/ecs.lua` declares `utf8`, `string.pack`, `string.unpack` and `string.packsize` for LuaLS, which does not know them in LuaJIT. Then `.github/scripts/check.sh` checks every Lua file with no new problems.
  8. **Workers' `stop`.** LuaJIT does not run hooks in the code it compiles, so the count hook of `Workers.c`, line 320, does not stop a loop that the JIT compiled: DESIGN 3.4 promises that `stop` ends a script "within a few thousand of its Lua instructions". Question 2 below decides it.
  9. **The documents.** DESIGN 0.3, 9, 10.3, 11, 17.1 and 17.4 and the glossary say LuaJIT and Lua 5.1 with LuaJIT's extensions, not Lua 5.5; so do the plugin skill, the template's README.md and GUIDE.md (shown to the owner first) and the examples' comments. Then measure again, as above, and write the result into the pull request.
  - **Questions for the owner**, to answer before step 1:
    1. Add lua-compat-5.3 as a dependency? It is maintained (lunarmodules, MIT, last change October 2026) and gives the C API forms, `utf8` and `string.pack` from Lua 5.3's own code. Without it, the core writes those itself: about 400 lines of `utf8` and `string.pack`, and about 15 small functions. Recommended: add it.
    2. Workers' `stop`: turn the JIT off in workers' states (`luaJIT_setmode`), so `stop` keeps its promise but workers run interpreted; or change DESIGN 3.4, so `stop` ends a script at its next `receive` or call of `ecs`, and a loop that calls neither runs until it ends. Recommended: the second, as workers exist for heavy work, where the JIT counts most.
- **More metatables, after LuaJIT.** Handles and structs already have methods and operators (DESIGN 0.6, principle 16; 10.11), and a panel's fields go through `__index` and `__newindex`. What LuaJIT adds:
  - **Operators.** A struct method named `mod`, `pow` or `concat` becomes `%`, `^` or `..`, as `add` becomes `+`: three entries in `OPENECS_STRUCT_OPERATORS` (`src/runtime/Services.c`) and DESIGN 10.11. `len` on a struct needs `LUAJIT_ENABLE_LUA52COMPAT`, which the build sets.
  - **Structs as FFI values.** A struct type already has C's layout (DESIGN 10.11), so the core could declare it with `ffi.cdef` and give it its methods with `ffi.metatype`. A vector would then be a value the JIT compiles, made without a table, and its operators would run as compiled code. A struct would be cdata, not a table, so DESIGN 10.11, saved state and `ecs.struct.new` change with it. Measure s10_cglm first, after the move to LuaJIT; do it only if the time is in structs.
  - **Classes in plugins.** A panel's state can be an object of its plugin's class, `setmetatable(state, Canvas)` with `Canvas.__index = Canvas`, so `state:draw()` reads as Lua does. The plugin skill and the template would show it.
  - Not: a proxy whose `__index` or `__newindex` calls something unseen, such as settings read as `ecs.settings.focus`. It breaks principle 4.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.
- **The core's libraries.** The standard plugins still use the core's SDL, SDL_ttf and stb_ds (DESIGN 17.2), against the principle that plugins stand alone (DESIGN 0.6). To remove: each plugin would bring what it uses.

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the owner's; changes go to it as CLAUDE.md says.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **GIFs of what OpenECS can do** in README.md, which the owner records.
- **GitHub settings**, which the owner sets for each of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template:
  - a description, one line on what the repository holds;
  - topics, such as `sdl3`, `lua`, `editor`, `plugin-system`, `c23` and `tooling`;
  - a social preview image, such as a screenshot of a tool with its panels;
  - issues labelled "good first issue".

Then announce in this order, each post written for its community: SDL's forum and the Handmade Network; r/C_Programming, r/lua, r/gamedev, r/linux and r/opensource; Show HN and Lobsters; the Lua mailing list. After a release, a short devlog, and a monthly "how I built X with OpenECS" post.

What would grow it most:

- **Releases for macOS**, since most tool builders are not on Linux. Windows has one since 0.4.0 (see Tasks).
- **A flagship tool** that is useful without writing code, such as a log viewer, a glTF viewer or a terminal, built from the standard plugins.
  - **An editor for LÖVE games**, the owner's flagship: [ONLR](https://github.com/omerfuyar/ONLR), in its own repository, whose TODO.md holds its open questions. Its files panel, viewer and console are standard plugins now.
- **A showcase** in README.md of plugins and tools others build.

## Review

Recommendations from a review of the code, by area, the most important first. P1 is a bug, or a design flaw that gets harder to fix with each plugin; P2 is worth doing before 1.0.0; P3 when the code is touched. Each was checked at the lines it names; none was proven by a run. Paths under `std/` are OpenECS-std's.

**Core: communication**

- **P3 · No example frees a handle while Lua holds it.** Nothing in OpenECS-examples calls `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**std: ui, with draw**

- **P3 · A column in a row measures its text at its parent's width.** `std/plugins/ui/init.lua` A row narrows its wrapped texts and columns only when its children need more than its width; a column that grows in a row still breaks its text at the width it had before it grew. Measure wrapped text again once widths are arranged. Size: M.
- **P3 · A long list or table is built whole each frame.** `std/plugins/ui/init.lua:536-545`, `:398-419` Each row is a node with two more for each cell, and a closure for each item with `onActivate`; all are measured and arranged, and only those in view are drawn. Rows have one height, so build and measure the rows in view, and share one handler among the items. A list takes children that are built already, so this needs a form of `ui.list` that takes a count and a function that builds one item; the owner leaves it until a tool needs it. Size: M.

**Core: plugins and threads**

- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. Size: M.

**Core: interface**

- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2848 lines hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The owner leaves it until it is necessary. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. Size: L.
- **P3 · No example presses two buttons, or sends input between a layout change and a frame.** `src/app/Test.c:845` `test.press` and `test.release` exist and no example uses them. Add cases for the two findings above that the tests can reach. Size: S.
