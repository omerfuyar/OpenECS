---
name: verify
description: Build and test OpenECS, OpenECS-std or OpenECS-examples, from a fresh checkout too. Use before a commit or a push, or when asked to build, test, check or verify a change. It sets up the submodules and packages, runs check.sh, the Debug build and the examples' tests, and reads failures without reading whole logs.
---

# Verify a change

Work from OpenECS's folder; OpenECS-std and OpenECS-examples are checked out in its `std/` and `examples/` (DESIGN.md, section 17.7).

## 1. Set up a fresh checkout, once

- If `dependencies/SDL/` is empty, run `git submodule update --init --recursive` (README.md).
- If there is no `build/Debug/`, install the packages with `.github/scripts/install-linux.sh`: gcc 14, cmake, ninja, SDL's system libraries and sway. Without sudo, as root, run its commands without `sudo`.
- The first Debug build compiles SDL and the other dependencies, which takes minutes: run it in the background. Later builds keep them.

## 2. Before each commit

Run the check script of the repository you changed: `.github/scripts/check.sh` in OpenECS, `std/` or `examples/`. It formats the changed C files and checks them with clang-tidy, checks the changed Lua files with the Lua language server, and prints only problems. clang-tidy needs a Debug build.

The Lua check runs only when `lua-language-server` is on the PATH. Install the version that the check `Lua (LuaLS)` uses, `LUALS_VERSION` in `.github/workflows/checks.yml`, once:

```sh
mkdir -p ~/.local/luals && curl -sSL https://github.com/LuaLS/lua-language-server/releases/download/3.15.0/lua-language-server-3.15.0-linux-x64.tar.gz | tar -xz -C ~/.local/luals
ln -sf ~/.local/luals/bin/lua-language-server ~/.local/bin/lua-language-server
```

## 3. Before pushing

1. `.github/scripts/build.sh D` builds Debug with the static analyzer and the sanitizers, and the examples. It prints the last 40 lines of `build/shuild.log` only when it fails.
2. `.github/scripts/test.sh` runs every example's test with shuild's `--test`, and prints the log of each test that fails.
3. Only when the change touches windows, popups or pop-outs: `.github/scripts/test-wayland.sh`.
4. Only when the change touches plugins' function signatures or their documentation: `.github/scripts/definitions.sh build/Debug/bin/OpenECS`.

The checks build Release, and run the tests on Wayland and Windows (DESIGN.md, section 19.3); do not run those yourself.

## Reading failures

- Search logs instead of reading them: `grep -nE "(error|warning):" build/shuild.log | grep -v dependencies/`.
- Run one test again: `build/Debug/bin/OpenECS --test build/Debug/bin/examples/<example>/test.lua`.
- A failure is a fact to explain, not a flake to run again. Fix its cause.
