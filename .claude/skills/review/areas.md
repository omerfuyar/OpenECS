# Areas

Each area: its files, its DESIGN.md sections (OpenECS's, or OpenECS-std's as std), and what to ask it beyond the review skill's checklist.

## Core: communication

How plugins talk to the core and to each other.

- Files: `src/runtime/Services.c`, `src/runtime/Events.c`, `src/base/Values.c`, `src/app/Bindings.c`, `include/OpenECS.h`, `include/ecs.lua`.
- DESIGN.md: 8, 10, 11.
- Ask:
  - What one service call costs, from C and from Lua: lookups by name, copies of values, libffi's preparation. What could be done once at lookup instead of at each call.
  - Handles: can a stale or wrong-type handle reach a plugin's code? What happens to handles and kept types when their plugin is disabled (9.10)?
  - Events: what happens when a handler subscribes, unsubscribes, emits or disables a plugin during delivery. Is the order of delivery defined and documented?
  - Do `OpenECS.h`, `ecs.lua` and Bindings.c say the same thing for each function (11.5)?
  - Errors in services (10.8): does every path report, and does a failed call leave no half-made values?

## Core: plugins and threads

- Files: `src/runtime/Plugins.c`, `src/runtime/Tasks.c`, `src/app/Workers.c`, `src/base/Lua.c`.
- DESIGN.md: 3, 9, 15.
- Ask: load order and failures half-way through loading; what disabling frees and what it keeps; what workers and tasks may touch from their threads.

## Core: interface

- Files: `src/interface/` (Window.c, Layout.c, Panels.c, Input.c, Keys.c, Menus.c, Popups.c, Dialogs.c).
- DESIGN.md: 4, 5, 6, 7.
- Ask: what is drawn or laid out again each frame without a change; how focus and pointer routing behave when a panel closes during an event; Window.c's size and what could leave it.

## Core: app

- Files: `src/app/App.c`, `src/app/Session.c`, `src/app/Desktop.c`, `src/app/Test.c`, `src/runtime/Settings.c`.
- DESIGN.md: 2, 12, 13, 14.
- Ask: start-up and shutdown when a step fails; sessions saved and read back the same; what a malformed user file does.

## std: ui

The plugin most tools build on.

- Files: `std/plugins/ui/init.lua`, `std/plugins/ui/header.h`, `std/plugins/ui/types.lua`, and draw: `std/plugins/draw/Draw.c`.
- std DESIGN.md: 1.4, 2, 5.
- Ask:
  - What `ui.show` costs for a panel of many elements: layout passes, text measured again, tables made each frame. What could be kept between frames.
  - Is every element usable with the keys alone, with a visible focus?
  - Are the elements' props named and behaving the same across elements (`selected`, `onSelect`, `onActivate`, `id`)?
  - Does `header.h` give C plugins all that Lua plugins have, with the same names?
  - Is `init.lua` one file for a reason, or would parts read better apart?

## std: other plugins

- Files: `std/plugins/<name>/`, one plugin at a time, and `std/shuild.c` for how it builds.
- std DESIGN.md: 1, and the plugin's own section.
- Ask: does it follow std's rules (1.3) and use no other plugin's dependencies; does its interface match the other plugins'; what its examples do not test.
