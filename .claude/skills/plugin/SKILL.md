---
name: plugin
description: Develop, check or audit an OpenECS plugin, in Lua or C - a standard plugin in OpenECS-std, an example's plugin in OpenECS-examples, or a tool's plugin made from OpenECS-template. Use when writing a new plugin, adding a panel type, service or setting to one, or checking that a plugin follows the design.
---

# A plugin

The design is in the documents; this is the order to use them in. Read only the sections named here, and those the task touches.

## 1. Read first

- OpenECS DESIGN.md 1, the conventions. For a standard plugin, OpenECS-std's DESIGN.md 1 too: its rules, names (1.3) and how panels draw (1.4).
- OpenECS DESIGN.md 9.1 and 9.2: a plugin is a folder with its manifest. 9.6 for Lua, 9.4 and 9.5 for C.
- OpenECS DESIGN.md 10.1 and 10.2 to register services with signatures, and 10.8 for their errors.
- The plugin's own section in OpenECS-std's DESIGN.md, or the example's comments, before changing it.
- An example that does the same is the best model: c1_hello and c2_native for the two kinds, c3_ui for ui, c4_settings, c5_services, c6_keys, c7_events and c8_state for the parts their names give; the examples' folders name the rest.

## 2. Write it

1. **The manifest** names the plugin, its version, description and the plugins it needs. A function of another plugin is found with `require`, never through a global.
2. **Services** have signatures that name each parameter, so the definition files and editors show them (DESIGN 10.10). A function that can fail outside its caller ends with `out string error` and fails with `ecs.fail` or `ECS_Fail`; a wrong argument is a misuse, checked with `ecs.check` or `ECS_Check`. A plugin never logs a failure it gives its caller.
3. **Panel types** draw into the surface they are given. A panel built with ui (std DESIGN 5) lets `ui.show` draw it, and draws nothing else into that surface, as ui draws only what changed. Sizes are in layout units.
4. **State.** A panel's saved state is a plain table with a version (OVERVIEW 10.3); the panel reads older versions.
5. **Settings and keys** are declared with types and defaults. A plugin binds keys only for its own panel types (OVERVIEW 7.3).
6. **Names** follow DESIGN 1.2 and 9.9. In C, static functions are `<Plugin><Verb><Object>` (std DESIGN 1.3), and clang-tidy checks them.
7. **Text for people**, such as descriptions and tooltips, is short and plain, as CLAUDE.md's Writing documents says.

## 3. Test it

- The examples are the tests (DESIGN 17.5). A plugin's behaviour is tested by an example's `test.lua`, whose preset names every plugin the test uses.
- The test API is in DESIGN 17.5: keys, text, clicks, the wheel, screenshots, settings and the session. Prefer checking state, such as `test.session()` or a service's result, over pixels.
- Build and run the tests as the verify skill says. Run the repository's `.github/scripts/check.sh` before each commit: it formats the C files, runs clang-tidy, and checks the Lua files with the Lua language server.

## 4. Audit it

Check in this order, and name the lines of each finding:

1. **Misuses:** element ids that two elements share, props that ui does not know, a missing id on an element that keeps state.
2. **Errors:** failures that are logged instead of given, misuses that are given as failures, handles used after `free`.
3. **Disabling:** the plugin disabled and enabled again while it works (DESIGN 9.10), and reloaded (9.11): no callback, timer, watch or thread outlives it.
4. **Drawing:** work done each frame that need not be, such as a whole redraw when a part changed, or a continuous panel that has nothing to animate.
5. **Parity:** a service written in Lua and called from C, and the other way round, behaves the same (DESIGN 11.5).
6. **Tests:** behaviour that no example's test covers.

Fix what the task covers. Record the rest in TODO.md, as the review skill says.
