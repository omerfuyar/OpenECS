# Notes

Notes for agents and the owner that belong in no other document: why the tools are set up as they are. They load only when read; AGENTS.md names this file.

## Tools

- shuild writes `compile_commands.json` in each repository's folder: the command that compiles each C file, of the last build. `.github/scripts/check.sh` runs clang-tidy on it, through a copy in `build/tidy/` without gcc's `-fanalyzer`, which clang does not have. clangd reads the same database, so editors with clangd, and Claude Code through a clangd plugin, see each file's real flags; git ignores `.clangd`, so each person's own drops gcc's flags with `CompileFlags: { Remove: [-fanalyzer, -fno-analyzer] }`.
- OpenECS's `.luarc.json` leaves `include/ecs.lua` out of the workspace, as the build's copy is in its library and LuaLS would see each class twice; the check `Lua (LuaLS)`, which runs without a build, checks it.
- The core's build gives the dependencies' headers as system headers (`-isystem`), so clang-tidy leaves their macros alone, such as stb_ds's `arrput`.
- `check.sh` checks Lua files with LuaLS over the whole folder and prints only the named files' problems, because LuaLS 3.15 finds nothing in a file that it checks alone.
- `check.sh` skips clang-tidy on `src/base/Sanitizers.c`: it includes gcc's sanitizer headers, which clang does not have.
- Checks that are off in `.clang-tidy`, because they only flagged ordinary C here:
  - `bugprone-multi-level-implicit-pointer-conversion`: `void *` with `memcpy`, `qsort` and stb_ds.
  - `bugprone-assignment-in-if-condition` and `bugprone-inc-dec-in-conditions`: deliberate, in chains of `||` and `&&`.
  - `bugprone-narrowing-conversions`: gcc's `-Wconversion` covers it.
  - `bugprone-branch-clone`: branches in a fixed order that do the same thing for different reasons.
  - `cert-err33-c`: unchecked `fprintf` and `fputs` to stderr. `SHUWUR` marks the results that must be checked.
  - `misc-include-cleaner`: the core includes through glue headers such as `stb/stbSDL3.h`.
- `readability-function-cognitive-complexity` ignores what macros expand to (`IgnoreMacros`). stb_ds's `arrput`, `SDL_assert`, Clay's `CLAY` and `SHUResult_String` expand into nested conditionals that a reader never sees; counted, they put almost every function that uses them over the threshold.
- Names that the naming checks cannot tell apart: a function pointer is PascalCase as a field, parameter or variable, so any PascalCase parameter or variable passes. A table of constants inside a function is UPPER_CASE, so any UPPER_CASE variable passes.
- clang-format splits chained conditionals, `a ? b : c ? d : e`, onto aligned lines; no option keeps them on one line without changing other code. `StatementMacros` names macros that make whole definitions, such as `ECSIMenus_WorkspaceFunctions`.
- The check `Format` uses clang-format 18, Ubuntu 24.04's. Another version may format some lines differently; format with 18.

## Working with agents

- One task per session; `/clear` between unrelated tasks. `/context` shows what fills the context.
- Plan mode for work across several files: review the plan before the code.
- Review in a fresh session or with `/code-review`, not in the session that wrote the code.
- `.claude/skills/` holds the procedures that only some tasks need: verify, release, submodule-bump and interface-change. They load only when a task needs them, so they cost nothing otherwise.
