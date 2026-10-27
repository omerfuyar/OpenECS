#!/bin/sh
# Formats OpenECS's C files with clang-format and checks them with clang-tidy (.clang-format, .clang-tidy), for agents before they commit.
# With no files named, it takes the C files of src/ and include/ that differ from dev, committed or not.
# It prints only the problems, and fails when clang-tidy finds one.
# clang-tidy reads the dependencies' headers from build/Debug/include, so it needs a Debug build first (build.sh D).
set -u
cd "$(dirname "$0")/../.." || exit 2

if [ $# -eq 0 ]; then
    base=$(git merge-base HEAD origin/dev 2> /dev/null || echo HEAD)
    # shellcheck disable=SC2046
    set -- $({
        git diff --name-only "$base"
        git ls-files --others --exclude-standard
    } | grep -E '^(src|include)/.*\.[ch]$' | sort -u)
fi

files=
for file in "$@"; do
    [ -f "$file" ] && files="$files $file"
done
[ -z "$files" ] && exit 0

# shellcheck disable=SC2086
clang-format -i $files || exit 2

if [ ! -d build/Debug/include ]; then
    echo "clang-tidy needs a Debug build first: .github/scripts/build.sh D"
    exit 2
fi

failed=0
for file in $files; do
    # Sanitizers.c includes gcc's sanitizer headers, which clang does not have
    case "$file" in
    */Sanitizers.c) continue ;;
    *.c) ;;
    *) continue ;;
    esac

    # the flags of the core's Debug build in shuild.c
    if ! clang-tidy --quiet "$file" -- -std=c23 -DOPENECS_CORE -DDEBUG -DSDL_ASSERT_LEVEL=2 \
        -Iinclude -isystem build/Debug/include -isystem build/Debug/include/libffi > build/tidy.log 2>&1; then
        grep -E "(warning|error):" -A3 build/tidy.log | grep -v -E "^[0-9]+ warnings? generated"
        failed=1
    fi
done

exit $failed
