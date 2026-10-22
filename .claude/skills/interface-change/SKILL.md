---
name: interface-change
description: Change, rename or remove anything plugins use - OpenECS.h, ecs.lua, the core's functions, settings, events, or the files of presets, sessions and settings - and carry the change through OpenECS-std, OpenECS-examples and OpenECS-template. Use for any change to the plugin interface or the ecs module.
---

# Change the plugin interface

The rules are in DESIGN.md: 11.5 parity of `OpenECS.h` and `ecs.lua`, 19.1 what breaks and which numbers change, 1.2 names. Read them first.

## 1. Find every use

Search every repository before you change anything, for example:

```sh
grep -rn "<name>" src include resources std/plugins std/presets examples ../OpenECS-template --include=*.c --include=*.h --include=*.lua --include=*.md
```

## 2. Change it in one piece of work

- In OpenECS: `include/OpenECS.h` and `include/ecs.lua` together, each `@lua` line, every reference in code, presets, settings files and documents, in the same commit.
- If native plugins must be built again, raise `OPENECS_API_VERSION`; if older OpenECS cannot read new preset or session files, raise their `format` (19.1).
- In OpenECS-std, OpenECS-examples and OpenECS-template: the same change on a branch of the same name. The examples are the tests, so a broken example is changed with it.

## 3. Pull requests, in order

1. OpenECS's pull request moves `std/` and `examples/` to the commits of their branches (the submodule-bump skill), so its checks build them together.
2. Merge OpenECS's first. The others' checks build OpenECS's `dev`, so they pass only after it.
3. Then OpenECS-std's, OpenECS-examples' and OpenECS-template's.
4. Then an OpenECS pull request moves `std/` and `examples/` to their `dev`.

Verify each with the verify skill before pushing.
