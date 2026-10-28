#!/bin/sh
# Runs the examples' tests with shuild's --test, after build.sh D compiled shuild: the examples are OpenECS's tests.
# shuild builds in Debug with the examples, runs each test in bin/examples/ and prints pass or FAIL for it;
# it fails if a test fails or a sanitizer reports a problem. SDL_VIDEO_DRIVER chooses another video driver, as test-wayland.sh does.
# Examples named after it, such as c12_sketch, run alone.
set -eu

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) compiler=clang extension=.exe ;;
*) compiler=gcc extension= ;;
esac

"./shuild.ignore$extension" -c "$compiler" --test "$@"
