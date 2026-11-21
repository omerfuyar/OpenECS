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
- **LuaJIT in place of Lua 5.5.** The owner decided it: OpenECS moves to LuaJIT 2.1, and its core changes where LuaJIT does a thing better. Measured in a Release build on Linux, with settings, keys, files and console redrawn whole 50 times a second, their Lua code took about 330 ms a second, draw's C 160 ms, and the calls between them about 3% of the Lua time (0.15 µs a call into C, against 0.03 µs into Lua; about a thousand calls a frame). LuaJIT makes the Lua code several times faster, so the calls' share grows, and a call through the C API also ends the JIT's trace; the FFI calls C from compiled code at C's speed. So the work has two parts, each one interface change (the interface-change skill), merged in the order of the pull-requests skill: first the move, which keeps the C API bridge and must pass every test; then the core's redesign, measured before and after with `ecs.profile` and LuaJIT's profiler.
  - **What LuaJIT 2.1 has.** Read its current `doc/extensions.html` and the notes of its LuaJIT 3.0 backports (https://github.com/LuaJIT/LuaJIT/issues/1475#issuecomment-4769582883) before each step; `v2.1` gains features often. Besides Lua 5.1, it has:
    - Syntax backported from LuaJIT 3.0, always on: `//`; the bit operators `~ & | << >> ~>>` with their metamethods (`__band`, `__shl`, ...) and `__idiv`; compound assignments (`+=`, `..=`, `//=`, `&=`, ...), whose metamethods get `true` as a third argument and may then change their first operand in place; `continue`; `?.`; `??`; `a ? b : c`; `const`; short functions (`x -> x + 1`); named varargs (`...args`); `1_000`.
    - From Lua 5.2 with `LUAJIT_ENABLE_LUA52COMPAT`: `__pairs`, `__ipairs`, `__len` on tables, `table.pack`, `table.unpack`, `rawlen`; always: `goto`, `load` with a mode and an environment, `table.move`, `xpcall` with arguments.
    - Its own libraries: `bit`, `ffi`, `jit`, `string.buffer` (fast buffers, and `encode`/`decode` of Lua values), `table.new`, `table.clear`, and a sampling profiler (`jit.profile`, and `luaJIT_profile_start` in C).
    - Not: `utf8`, `string.pack`, integers. Bit operators work on 32-bit numbers, as `bit` does, and give signed results: `0xFF000000 | 0` is negative.
    - LuaLS 3.19.0 and later parse all of it with `"runtime.version": "LuaJIT"` and `"runtime.enableLuaJITExtensions": true`.
  - **Part 1, the move.**
    1. **The dependency.** Add `dependencies/luajit`, from https://github.com/LuaJIT/LuaJIT, branch `v2.1`, which LuaJIT releases from directly. Add `dependencies/lua-compat-5.3`, from https://github.com/lunarmodules/lua-compat-5.3 (MIT; the owner agreed), for the C API's 5.3 forms and `utf8`. It stays as long as OpenECS uses LuaJIT, compiled into the core like LuaJIT itself: plugins see only the `utf8` library it gives, and nothing of the C header (DESIGN 0.6, principle 7). Remove `dependencies/lua`. Update DESIGN 17.1 and 17.4, README.md and the licences the release ships.
    2. **The build.** `Shuild_luajit` in `shuild.c` replaces `Shuild_lua`. It does what LuaJIT's `src/Makefile` does, and writes every generated file under the build's folder, never into `dependencies/`:
       1. Build the host tool `minilua` from `src/host/minilua.c`, with the host's compiler and `-lm`.
       2. Run `minilua dynasm/dynasm.lua FLAGS -o buildvm_arch.h src/vm_x64.dasc`. The flags are `DASM_AFLAGS` of the Makefile (lines 406 to 488), which it reads from the target's `lj_arch.h` with `cc -E -dM`: on x64 `-D ENDIAN_LE -D P64 -D JIT -D FFI -D FPU -D HFABI -D VER=`, and `-D WIN` for Windows. Read them the same way, not as a fixed list, so aarch64 works later.
       3. Write `luajit.h` from `src/luajit_rolling.h` with `minilua src/host/genversion.lua`, which takes the release number from `luajit_relver.txt`: the time of the submodule's commit (`git show -s --format=%ct`), or `.relver`.
       4. Build the host tool `buildvm` from `src/host/buildvm*.c`, with the folder of `buildvm_arch.h` on its include path. For a cross build, the host tools get the target's `-DLUAJIT_TARGET` and `-DLUAJIT_OS`, as the Makefile's `HOST_XCFLAGS` does.
       5. Run `buildvm`: `-m elfasm -o lj_vm.S` on Linux, `-m peobj -o lj_vm.o` on Windows; `-m bcdef`, `-m ffdef`, `-m libdef` and `-m recdef` over the `LJLIB_C` files (Makefile, line 512); `-m folddef` over `lj_opt_fold.c`; and `-m vmdef -o jit/vmdef.lua`.
       6. Build the static library `luajit` from `src/ljamalg.c` and `lj_vm.S` or `lj_vm.o`, with `-DLUAJIT_ENABLE_LUA52COMPAT` and the generated folder first on the include path; in Debug, also `-DLUA_USE_APICHECK` and `-DLUA_USE_ASSERT`. Copy `lua.h`, `lauxlib.h`, `lualib.h`, `luaconf.h`, `luajit.h` and `lua.hpp` to `<dependencies>/include/lua/`, so the core's includes stay. Copy `src/jit/*.lua` and `vmdef.lua` next to the program as `jit/`, so `require("jit.p")`, `jit.v` and `jit.dump` work for whoever studies traces.
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
    4. **Numbers.** LuaJIT has only doubles, exact as integers up to 2^53. A number is an integer when it is whole: lua-compat-5.3's `lua_isinteger` tests that. Settings of type `integer`, `int` parameters and `ECSValue` (DESIGN 10.3) read a whole number as an integer. The core's writer (`OPENECS_LUA_WRITER`, `src/base/Lua.c`, line 34) tests `n % 1 == 0` in place of `math.type`. Write the rule into DESIGN 10.3 and 11. `i64` parameters take a whole number, or an `int64_t` cdata (`123LL`) for more than 2^53.
    5. **utf8.** DESIGN 9 promises `utf8`, which LuaJIT lacks. The core builds lua-compat-5.3's `lutf8lib.c` as `utf8`.
    6. **The Lua code**, in std, examples, the template and `include/ecs.lua`. Most of it stays, as LuaJIT parses `//` and the bit operators. Find each with grep over `*.lua`:
       - Bit operators: check every use for the 32-bit signed results: ui's colours, c12_sketch, s6_web's SHA-1, s8_gpu and s10_cglm. A colour above `0x7FFFFFFF` comes out negative; where a result must stay positive, add `% 2^32`.
       - `math.type(n) == "integer"` becomes `n % 1 == 0`, and `math.tointeger` likewise: std's settings, console and ui.
       - `string.pack` and `string.unpack` (std's ui and gpu; s2_media, s4_model, s6_web, s8_gpu, s9_gpu_model and s10_cglm) become FFI arrays, `ffi.new("float[?]", count)`, given to the service as a buffer (Part 2, step 3); until then, `ffi.string(array, ffi.sizeof(array))`.
    7. **Editors and checks.** Each repository's `.luarc.json` sets `"runtime.version": "LuaJIT"` and `"runtime.enableLuaJITExtensions": true`. `LUALS_VERSION` in `.github/workflows/checks.yml` becomes 3.19.2 or newer, and so does the LuaLS that agents install. `include/ecs.lua` declares `utf8` for LuaLS.
    8. **Workers' `stop`.** LuaJIT does not run hooks in the code it compiles, so the count hook of `Workers.c`, line 320, cannot stop a loop that the JIT compiled. The owner decided: workers keep the JIT, and `stop` ends a script at its next `receive` or call of `ecs`; a loop that calls neither runs until it ends. Remove the hook, and change DESIGN 3.4, which promises "within a few thousand of its Lua instructions".
    9. **The documents.** DESIGN 0.3, 9, 10.3, 11, 17.1 and 17.4 and the glossary say LuaJIT, not Lua 5.5. DESIGN 1 gets the Lua conventions: plugins write LuaJIT's syntax where it is shorter or safer, such as `+=`, `continue`, `??` for defaults that may be `false`, `?.`, `//`, the bit operators and `const` for constants. The plugin skill, the template's README.md and GUIDE.md (shown to the owner first) and the examples follow. Then measure again, as above, and write the result into the pull request.
  - **Part 2, the core's redesign.** Each step is its own pull request after Part 1, with DESIGN changed first and a measurement of the case it speeds up.
    1. **Lua calls C through the FFI** (DESIGN 10.4, 10.9). For each registered C function, the core makes a Lua wrapper from its signature: an `ffi.cdef` of the function's type, the function pointer cast to it, and Lua code for what the C bridge does today: misuse checks, `out` parameters, `out string error`, callbacks and the profiler's scopes (DESIGN 3.5), which call the core through the FFI too. The JIT compiles the wrapper with its caller, so ui's calls into draw run as C calls. C calls Lua through libffi's closures as today: LuaJIT's FFI callbacks are few and not compiled.
    2. **Handles as cdata.** A handle type becomes a C pointer type of its own (`struct ecs_handle_<name> *`), with its methods by `ffi.metatype` and its destructor by `ffi.gc`. The FFI then rejects a handle of another type, so the core's check in Lua goes (DESIGN 10.6).
    3. **Buffers from the FFI.** A `buffer` parameter also takes a cdata pointer with its length, and a `string.buffer`, without a copy (DESIGN 10.7). Plugins then build vertices and pixels in FFI arrays.
    4. **Structs as cdata.** A struct type is already laid out as C (DESIGN 10.11): `ffi.cdef` declares it, and `ffi.metatype` gives it its methods and operators. A function takes the cdata by pointer, without converting fields; a table of fields still works, through `ffi.new`. The operators add `idiv` (`//`), `mod`, `pow`, `concat` and the bit operators; an operator method may change its first operand in place when it gets `true` as a third argument, so `position += velocity` makes no new struct. Saved state and values (DESIGN 10.3, 13.4) write a struct as a table of its fields.
    5. **Workers' values through `string.buffer`.** `send` and `receive` encode and decode values with `string.buffer` in place of the copy in `Workers.c`; FFI cdata such as structs cross as well. A function or a handle in a value is an error, not skipped (DESIGN 3.4).
    6. **Lua's functions in the profile.** While `ecs.profile` is on, the core runs LuaJIT's sampling profiler on the main state, and `ecs.profile.get()` adds `functions`: the Lua functions where the last second went, with their plugin (DESIGN 3.5). std's profiler shows them.
    7. **ui.** With the tools above, rebuild ui's tree with `table.new` and reused tables (`table.clear`), and check with `jit.p` and `jit.v` that its building and arranging are compiled, not interpreted.
- **Classes in plugins.** A panel's state can be an object of its plugin's class, `setmetatable(state, Canvas)` with `Canvas.__index = Canvas`, so `state:draw()` reads as Lua does. The plugin skill and the template would show it. Not a proxy whose `__index` or `__newindex` calls something unseen, such as settings read as `ecs.settings.focus`: it breaks principle 4.

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
