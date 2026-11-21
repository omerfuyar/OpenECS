---
name: plugin
description: Write, check or audit an OpenECS plugin, in Lua or C, in OpenECS-std, OpenECS-examples, the template or a tool made from it. Use when making or changing a plugin, or when asked to audit one.
---

# A plugin

DESIGN.md holds the rules; this skill names the sections each step needs and what to check. Paths are OpenECS's; `std/` is OpenECS-std, `examples/` is OpenECS-examples.

## 1. Read only what the plugin needs

- Always: DESIGN.md 9.1, 9.2, 9.9 and 9.10 (folder, manifest, names, disabling), 1.4 (failures and misuses) and 15 (ownership).
- A panel: 4.1 to 4.3, and 5.1. A service: 10.1 to 10.8. Lua: 1.1, for its conventions, 9.6, for the libraries it has, and 11. C: 1.2, 1.3 and 1.6, and std's DESIGN.md 1.3 for a plugin's own function names.
- In OpenECS-std: std's DESIGN.md 1 whole, and the section of each plugin it uses.
- The example that teaches a part, read before writing it: `c1_hello` a Lua panel, `c2_native` a C plugin, `c3_ui` ui, `c4_settings` settings, `c5_services` services, `c6_keys` keys, `c7_events` events, `c8_state` saved state, `c9_drag_drop` dragged data, `c10_text_input` typing, `c11_popups` popups, `c12_sketch` drawing only what changed, `c13_workers` threads, `c14_structs` structs. The `s` examples each show a standard plugin.
- Signatures, not source: `include/OpenECS.h` and `include/ecs.lua` for the core, and a built plugin's definition header, `build/Debug/include/<plugin>.h`, for another plugin's functions.

## 2. Write it

- Where it goes: in OpenECS-std, `plugins/<name>/`, shown and tested by an example (std DESIGN 1.2); in an example, a folder beside its preset (DESIGN 17.6); in a tool from the template, `plugins/<name>/`, tested by its `test.lua`.
- Lua unless the plugin needs a C library or speed that Lua lacks. A C plugin is built from every C file in its folder (DESIGN 17.2), so it needs no build settings.
- A Lua panel type is a class, and its state an object of it: `create` gives `setmetatable({ panel = panel }, Class)`, and the type names the methods, `draw = Class.draw` (DESIGN 1.1). No `__index` function or `__newindex` that does work out of sight.
- Declare by local names; look up, emit, subscribe and bind by full names (9.9). Name each dependency in the manifest, and get its functions with `require` (10.5).
- A failure from outside gives `nil, message`, or `out string error` last for a service; a wrong argument is a misuse, checked with `ecs.check` or `ECS_Check` (1.4). A plugin never logs a failure that it returns.
- Every call into the core costs about a third of a microsecond in a Debug build, so a Lua panel paints runs of pixels with `surface:fill` and rows with `setRow`, as c12_sketch's Lua panel paints each row of a dab with one `fill`. A panel that keeps its picture (`kept`) draws only what changed, and names it with `surface:changed` (5.1). ui does both for the panels it shows.
- Give an id to each ui element that keeps state or that the tree may move: a scroll, table, tree, field or key needs one, and in C two buttons with one label need their own (std DESIGN 5).

## 3. Check it

- The check script of the repository it lives in, before each commit: OpenECS's, std's or examples' `.github/scripts/check.sh`, or the template's. It formats the C files and runs clang-tidy and the Lua language server.
- A test in the examples' manner (DESIGN 17.5): a file `test*.lua` that returns `preset` and `run(test)`, and drives the plugin as the user would, with `test.key`, `test.click`, `test.text` and `test.wait`, then checks with `test.match`, `test.panel`, `test.pixel` and `test.session`. It checks behaviour the plugin promises, not its internals.
- In OpenECS, std or examples: the verify skill. In a tool: `cc shuild.c -o shuild && ./shuild`, then `OpenECS/OpenECS --test test.lua`.
- A change to the plugin's functions or their documentation: `.github/scripts/definitions.sh build/Debug/bin/OpenECS` in OpenECS.
- Its speed: the profiler (Alt+W, then F; std DESIGN 32) shows the plugin's own time, its panels' draws and its longest call. Its own time leaves out the C plugins it calls, but not the Lua plugins, which run in the same Lua call (DESIGN 3.5).

## 4. Audit it

Read it against this list, in this order, and name the lines for each finding:

1. **Misuses.** Every argument a caller gives is checked with `ecs.check` or `ECS_Check`: types, `nil` and `nullptr` handles, ranges, sizes that do not fit.
2. **Failures.** Each failure from outside is returned with why, and not logged as well. Error paths free what they made and leave no half state.
3. **Lifetimes.** Nothing keeps a surface after `Draw`. Handles are freed or forgotten as 10.6 says. A plugin disabled and enabled again (9.10) leaves no timer, worker, subscription or binding behind.
4. **Names.** Local names when declaring, full names elsewhere (9.9); C names as 1.2 and std DESIGN 1.3 say. ui elements that keep state have ids.
5. **Drawing.** No whole redraw where `kept` allows a part; `surface:changed` names what changed; no call into the core per pixel.
6. **Calls written out.** Functions are called and registered by the names a reader can find (DESIGN 0.6, "Calls are written out").
7. **Tests.** Each promise of the plugin is checked by its example's test.

Fix what the audit finds with the plugin, or, for OpenECS and OpenECS-std, record it as the review skill says.
