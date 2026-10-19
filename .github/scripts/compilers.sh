#!/bin/sh
# Builds and runs shuild's example 16_compiler_detection, so shuild's compiler detection runs on each system the checks run on.
# It fails when shuild finds no compiler for this system, or when the example it builds with the compiler it picked does not run.
# CC names the compiler that builds the example's build script, gcc if it is not set.
set -eu

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) system=Windows extension=.exe ;;
*) system=Linux extension= ;;
esac

# shuild finds the example's files beside its build script's executable
cd dependencies/shuild/examples/16_compiler_detection
rm -f "example$extension"
"${CC:-gcc}" -DSHU_IMPLEMENTATION shuild.c -o "shuild.ignore$extension"
status=0
"./shuild.ignore$extension" > shuild.ignore.log 2>&1 || status=$?
cat shuild.ignore.log
[ "$status" -eq 0 ] || exit "$status"

if ! grep -q "for $system" shuild.ignore.log; then
    echo "shuild found no compiler for $system."
    exit 1
fi

"./example$extension"
