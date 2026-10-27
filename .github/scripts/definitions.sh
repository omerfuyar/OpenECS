#!/bin/sh
# Writes the plugin interface and the definition files of the plugins of the example 5_services, one in C and one in Lua, with the Debug executable, and checks
# that every C header compiles with that folder alone, and that every function of the standard plugins says what each of its parameters and its result is.
# CC names the compiler, gcc if it is not set.
set -eu

executable="$1"
folder=build/definitions
rm -rf "$folder"

"$executable" --preset "$(dirname "$executable")/examples/5_services/preset.lua" --definitions "$folder" > build/definitions.log 2>&1

for header in "$folder"/*.h; do
    printf '#include "%s"\n' "$(basename "$header")" | "${CC:-gcc}" -std=c23 -Wall -Wextra -Werror -fsyntax-only -I"$folder" -x c -
    echo "compiles $header"
done

# every function of the standard plugins says what each of its parameters and its result is (DESIGN 10.1): the build wrote their definition headers
# into include/, beside the plugin interface, whose own documentation is not a definition file's
for header in "$(dirname "$executable")"/../include/*.h; do
    case "$(basename "$header")" in OpenECS.h | shu.h) continue ;; esac
    awk -v file="$(basename "$header")" '
        /^ *\/\/\// { if ($2 == "@param") documented[$3] = 1; if ($2 == "@return") returns = 1; next }
        /\(\*[A-Za-z_0-9]+\)\(.*\);$/ {
            line = $0
            sub(/^ +/, "", line)
            result = line; sub(/ *\(\*.*/, "", result)
            name = line; sub(/^[^(]*\(\*/, "", name); sub(/\).*/, "", name)
            parameters = line; sub(/^[^(]*\(\*[A-Za-z_0-9]+\)\(/, "", parameters); sub(/\);$/, "", parameters)
            # a callback parameter, void (*done)(int value), keeps its name, and its own parameters, with their commas, go
            while (match(parameters, /\(\*[A-Za-z_0-9]+\)/)) parameters = substr(parameters, 1, RSTART - 1) substr(parameters, RSTART + 2, RLENGTH - 3) substr(parameters, RSTART + RLENGTH)
            while (match(parameters, /\([^()]*\)/)) parameters = substr(parameters, 1, RSTART - 1) substr(parameters, RSTART + RLENGTH)
            count = parameters == "void" || parameters == "" ? 0 : split(parameters, list, ",")
            for (i = 1; i <= count; i++) {
                parameter = list[i]
                sub(/ +$/, "", parameter)
                sub(/.*[^A-Za-z_0-9]/, "", parameter)
                if (!(parameter in documented)) { print file ": " name ": no @param " parameter; missing = 1 }
            }
            if (result != "void" && !returns) { print file ": " name ": no @return"; missing = 1 }
            delete documented
            returns = 0
            next
        }
        { delete documented; returns = 0 }
        END { exit missing }
    ' "$header"
done
echo "every function of the standard plugins documents its parameters"
