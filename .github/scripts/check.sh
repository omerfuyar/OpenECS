#!/bin/sh
# Formats OpenECS's C files with clang-format and checks them with clang-tidy (.clang-format, .clang-tidy), and checks its Lua files with the Lua
# language server when it is installed, for agents before they commit.
# With no files named, it takes the C files of src/ and include/ and the Lua files that differ from dev, committed or not.
# It prints only the problems, and fails when clang-tidy or the Lua language server finds one.
# clang-tidy reads each file's compile command from the compilation database that shuild writes, so it needs a Debug build first (build.sh D).
set -u
cd "$(dirname "$0")/../.." || exit 2

if [ $# -eq 0 ]; then
    base=$(git merge-base HEAD origin/dev 2> /dev/null || echo HEAD)
    # shellcheck disable=SC2046
    set -- $({
        git diff --name-only "$base"
        git ls-files --others --exclude-standard
    } | grep -E '^((src|include)/.*\.[ch]|.*\.lua)$' | sort -u)
fi

files=
lua=
for file in "$@"; do
    case "$file" in
    *.lua) [ -f "$file" ] && lua="$lua $file" ;;
    *) [ -f "$file" ] && files="$files $file" ;;
    esac
done

failed=0

# LuaLS checks one file only as part of its folder, so it checks the whole folder, as the check Lua (LuaLS) does, and the problems of the files named here are printed;
# its lines name a problem's file first, such as plugins/ui/init.lua:2:11 [Warning] ..., and the two lines after show where
if [ -n "$lua" ] && command -v lua-language-server > /dev/null; then
    mkdir -p build
    lua-language-server --check="$PWD" --checklevel=Warning --check_format=pretty --logpath=build/luals > build/luals.log 2>&1
    if tr '\r' '\n' < build/luals.log | sed 's/\x1b\[[0-9;]*m//g' | awk -v files="$lua" '
        BEGIN { split(files, list, " "); for (i in list) named[list[i]] = 1 }
        /:[0-9]+:[0-9]+ \[/ { split($0, parts, ":"); show = parts[1] in named; if (show) { found = 1; print }; next }
        /^ +[^ ]/ && show { print; next }
        { show = 0 }
        END { exit !found }'; then
        failed=1
    fi
fi

[ -z "$files" ] && exit $failed

# shellcheck disable=SC2086
clang-format -i $files || exit 2

# shuild writes the compile command of each C file into compile_commands.json; clang has no -fanalyzer, gcc's static analyzer, so clang-tidy reads a copy without it
if [ ! -f compile_commands.json ]; then
    echo "clang-tidy needs the compilation database of a Debug build first: .github/scripts/build.sh D"
    exit 2
fi
mkdir -p build/tidy
sed 's/"-f\(no-\)\{0,1\}analyzer", //g' compile_commands.json > build/tidy/compile_commands.json

for file in $files; do
    # Sanitizers.c includes gcc's sanitizer headers, which clang does not have
    case "$file" in
    */Sanitizers.c) continue ;;
    *.c) ;;
    *) continue ;;
    esac

    if ! clang-tidy --quiet -p build/tidy "$file" > build/tidy.log 2>&1; then
        grep -E "(warning|error):" -A3 build/tidy.log | grep -v -E "^[0-9]+ warnings? generated"
        failed=1
    fi
done

exit $failed
