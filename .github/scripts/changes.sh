#!/bin/sh
# Tells the checks, in GITHUB_OUTPUT, what the other jobs need first; the job changes runs it.
# code: false when only documents changed, so the builds and tests are skipped. Documents are the .md files of the root, .claude/ and .github/;
# an .md file elsewhere, such as an example's, may be read by code. A run by hand, or an answer git cannot give, says true.
# Each argument NAME=REPOSITORY, such as openecs=OpenECS, gives NAME: on a pull request, its branch's name when the owner's REPOSITORY has a
# branch of the same name, so a change that spans repositories is checked with their branches; otherwise nothing.
# BEFORE names the commit that a push moved its branch from.
set -u
cd "$(dirname "$0")/../.." || exit 2

range=
case "$GITHUB_EVENT_NAME" in
# the pull request's merge commit, whose first parent is its base
pull_request) range="HEAD^1 HEAD" ;;
push) git fetch -q --depth=1 origin "${BEFORE:-}" 2> /dev/null && range="$BEFORE $GITHUB_SHA" ;;
esac

code=true
# shellcheck disable=SC2086
if [ -n "$range" ] && files=$(git diff --name-only $range) && [ -n "$files" ]; then
    echo "$files" | grep -q -v -E '^([^/]+|\.claude/.+|\.github/.+)\.md$' || code=false
fi
echo "code=$code" | tee -a "$GITHUB_OUTPUT"

for pair in "$@"; do
    branch=
    if [ "$GITHUB_EVENT_NAME" = pull_request ] && git ls-remote --exit-code "https://github.com/omerfuyar/${pair#*=}.git" "refs/heads/$GITHUB_HEAD_REF" > /dev/null; then
        branch=$GITHUB_HEAD_REF
    fi
    echo "${pair%%=*}=$branch" | tee -a "$GITHUB_OUTPUT"
done
