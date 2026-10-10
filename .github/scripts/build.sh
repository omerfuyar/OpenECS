#!/bin/sh
# Builds OpenECS with shuild: D for Debug with the examples and the tests, R for Release with the examples.
# Shuild's exit status does not tell whether the build worked, so this fails on a compiler error or warning in OpenECS's own files, or a missing executable.
set -eu

type="$1"
case "$type" in
D) folder=Debug flags="-b debug -e -t" ;;
R) folder=Release flags="-b release -e" ;;
*) echo "Usage: build.sh D|R" && exit 2 ;;
esac

executable="build/$folder/bin/OpenECS"
rm -f "$executable"

mkdir -p build
gcc shuild.c -o shuild.ignore -O3
# shellcheck disable=SC2086
./shuild.ignore $flags > build/shuild.log 2>&1 || true

# dependencies are compiled without OpenECS's warnings, so only the problems in its own files count, named from the repository's root or by their full path
root=$(pwd)
if grep -E "^(\./|$root/)?(src|include|std|examples|tests)/[^:]*:[0-9]+:[0-9]+: (fatal error|error|warning):|^gcc: fatal error|(shuild\.(c|h)|build\.c):[0-9]+:.*ERROR" build/shuild.log; then
    echo "The build has errors or warnings; build/shuild.log has the details."
    exit 1
fi

if [ ! -x "$executable" ]; then
    tail -n 40 build/shuild.log
    echo "The build made no executable."
    exit 1
fi
