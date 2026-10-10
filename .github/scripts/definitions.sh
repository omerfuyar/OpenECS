#!/bin/sh
# Writes the definition files of the test plugins' functions with the Debug executable, and checks that every C header compiles.
set -eu

executable="$1"
folder=build/definitions
rm -rf "$folder"

"$executable" --preset "$(dirname "$executable")/../tests/presets/callbacks.lua" --definitions "$folder" > build/definitions.log 2>&1

for header in "$folder"/*.h; do
    printf '#include "%s"\n' "$(basename "$header")" | gcc -std=c23 -Wall -Wextra -Werror -fsyntax-only -Iinclude -Idependencies -I"$folder" -x c -
    echo "compiles $header"
done
