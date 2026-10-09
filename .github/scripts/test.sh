#!/bin/sh
# Runs every test in tests/ with a Debug executable, and fails if a test fails or a sanitizer reports a problem.
set -u

executable="$1"
failed=0

for test in tests/*.lua; do
    if "$executable" --test "$test" > build/test.log 2>&1 && ! grep -q -E 'Sanitizer|runtime error:' build/test.log; then
        echo "pass $test"
    else
        echo "FAIL $test"
        cat build/test.log
        failed=1
    fi
done

exit $failed
