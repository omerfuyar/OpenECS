#!/bin/sh
# Writes the definition files of the plugins of the example 5_services, one in C and one in Lua, with the Debug executable, and checks that every C header compiles.
# CC names the compiler, gcc if it is not set.
set -eu

executable="$1"
folder=build/definitions
rm -rf "$folder"

"$executable" --preset "$(dirname "$executable")/examples/5_services/preset.lua" --definitions "$folder" > build/definitions.log 2>&1

for header in "$folder"/*.h; do
    printf '#include "%s"\n' "$(basename "$header")" | "${CC:-gcc}" -std=c23 -Wall -Wextra -Werror -fsyntax-only -Iinclude -Idependencies -I"$folder" -x c -
    echo "compiles $header"
done
