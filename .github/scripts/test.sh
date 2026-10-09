#!/bin/sh
# Runs the tests that the build copied beside bin/ with its Debug executable, or with "examples" the examples' tests,
# and fails if a test fails or a sanitizer reports a problem.
set -u

executable="$1"
bin=$(dirname "$executable")
failed=0

if [ "${2:-}" = "examples" ]; then
    tests="$bin/examples/*/test*.lua"
else
    tests="$bin/../tests/*.lua"
fi

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
