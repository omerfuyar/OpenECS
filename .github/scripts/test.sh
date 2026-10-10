#!/bin/sh
# Runs the tests of one repository with a Debug executable: OpenECS's own, the ones the build copied into tests/ beside bin/;
# with "std" OpenECS-std's, in std/tests/ beside bin/; with "examples" the examples' tests, in bin/examples/.
# Fails if a test fails or a sanitizer reports a problem.
set -u

executable="$1"
bin=$(dirname "$executable")
failed=0

case "${2:-}" in
"") tests="$bin/../tests/*.lua" ;;
std) tests="$bin/../std/tests/*.lua" ;;
examples) tests="$bin/examples/*/test*.lua" ;;
*) echo "Usage: test.sh EXECUTABLE [std|examples]" && exit 2 ;;
esac

for test in $tests; do
    if "$executable" --test "$test" > build/test.log 2>&1 && ! grep -q -E 'Sanitizer|runtime error:' build/test.log; then
        echo "pass $test"
    else
        echo "FAIL $test"
        cat build/test.log
        failed=1
    fi
done

exit $failed
