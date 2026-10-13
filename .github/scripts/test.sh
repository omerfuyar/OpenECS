#!/bin/sh
# Runs the examples' tests with a Debug executable: the examples are OpenECS's tests, and the build copies them into bin/examples/.
# Fails if a test fails or a sanitizer reports a problem.
set -u

executable="$1"
bin=$(dirname "$executable")
failed=0

for test in "$bin"/examples/*/test*.lua; do
    if "$executable" --test "$test" > build/test.log 2>&1 && ! grep -q -E 'Sanitizer|runtime error:' build/test.log; then
        echo "pass $test"
    else
        echo "FAIL $test"
        cat build/test.log
        failed=1
    fi
done

exit $failed
