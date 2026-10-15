#!/bin/sh
# Builds OpenECS with shuild and the examples: D for Debug, R for Release.
# This fails when shuild fails, on a compiler warning in OpenECS's own files, or when there is no executable.
set -eu

type="$1"
case "$type" in
D) folder=Debug flags="-b debug -e" ;;
R) folder=Release flags="-b release -e" ;;
*) echo "Usage: build.sh D|R" && exit 2 ;;
esac

executable="build/$folder/bin/OpenECS"
rm -f "$executable"

mkdir -p build
# the flags are shuild's SHUILD_BUILD, which shuild uses when it builds itself again
gcc shuild.c -o shuild.ignore -O3 -Wno-format-truncation
# shuild stops with the exit code of a command that failed
# shellcheck disable=SC2086
if ! ./shuild.ignore $flags > build/shuild.log 2>&1; then
    tail -n 40 build/shuild.log
    echo "The build failed; build/shuild.log has the details."
    exit 1
fi

# dependencies are compiled without OpenECS's warnings, so only the problems in the files of OpenECS, OpenECS-std and OpenECS-examples count;
# std and examples are built in their own folders, so a file may be named from there or by its full path;
# a warning in a system header, such as glibc's memcpy in Lua's code, is left out too, as the dependencies' code is what gets inlined there
if grep -E "^[^ :]+:[0-9]+:[0-9]+: (fatal error|error|warning):" build/shuild.log | grep -v -E "^(([^ :]*/)?dependencies/|/usr/)" ||
    grep -E "^gcc: fatal error|shuild\.(c|h):[0-9]+:.*ERROR" build/shuild.log; then
    echo "The build has errors or warnings; build/shuild.log has the details."
    exit 1
fi

if [ ! -x "$executable" ]; then
    tail -n 40 build/shuild.log
    echo "The build made no executable."
    exit 1
fi
