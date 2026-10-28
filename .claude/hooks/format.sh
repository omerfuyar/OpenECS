#!/bin/sh
# Claude Code runs this after each edit of a file (.claude/settings.json): it formats a C file of OpenECS with clang-format.
# It prints nothing, so it costs no context; without clang-format it does nothing.
file=$(sed -n 's/.*"file_path" *: *"\([^"]*\)".*/\1/p')

case "$file" in
*/dependencies/*) exit 0 ;;
*.c | *.h) command -v clang-format > /dev/null && clang-format -i "$file" ;;
esac

exit 0
