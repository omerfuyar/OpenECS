#!/bin/sh
# Claude Code runs this when a session starts (.claude/settings.json). In a cloud session only, it installs what the build, the checks and the
# tests need, as CI has them, and checks out the submodules, so the first Debug build and check.sh work at once. Each step is skipped when it is done.
set -eu

[ "${CLAUDE_CODE_REMOTE:-}" = "true" ] || exit 0
cd "${CLAUDE_PROJECT_DIR:-$(dirname "$0")/../..}"

# the packages of install-linux.sh, which CI installs, and those that clang, clang-tidy and clangd need here: gcc 14's C++ library for the
# C++ libraries that std builds with clang, and clang's sanitizer runtime for a clang Debug build
if ! command -v gcc-14 > /dev/null || ! command -v sway > /dev/null; then
    .github/scripts/install-linux.sh > /dev/null 2>&1
fi
if ! dpkg -s libstdc++-14-dev libclang-rt-18-dev clang-format-18 clang-tidy-18 clangd-18 > /dev/null 2>&1; then
    sudo apt-get install -y libstdc++-14-dev libclang-rt-18-dev clang-format-18 clang-tidy-18 clangd-18 > /dev/null 2>&1
fi

# the Lua language server of the check Lua (LuaLS), which check.sh runs on changed Lua files
luals=$(sed -n 's/.*LUALS_VERSION: *//p' .github/workflows/checks.yml | head -n 1)
if [ "$(lua-language-server --version 2> /dev/null)" != "$luals" ]; then
    sudo mkdir -p /opt/luals
    curl -sSL "https://github.com/LuaLS/lua-language-server/releases/download/$luals/lua-language-server-$luals-linux-x64.tar.gz" | sudo tar -xz -C /opt/luals
    sudo ln -sf /opt/luals/bin/lua-language-server /usr/local/bin/lua-language-server
fi

# the dependencies, std/ and examples/, with their own submodules
if [ ! -f dependencies/SDL/CMakeLists.txt ] || [ ! -f std/shuild.c ]; then
    git submodule update --init --recursive > /dev/null 2>&1
fi

exit 0
