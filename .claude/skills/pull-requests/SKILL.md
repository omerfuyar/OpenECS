---
name: pull-requests
description: Open, follow and merge the pull requests of a change across OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template, in the order their checks need. Use after verifying a change, and when opening, merging or following its pull requests.
---

# Pull requests across the repositories

CLAUDE.md, section Git, holds the rules: branches from `dev`, merge commits, never `main`. This is the order that makes every check pass, and how to wait for them cheaply.

## 1. The order

OpenECS's checks build `std/` and `examples/` at the commits its submodules name. The checks of the other three build OpenECS's `dev`. So:

- **A change in one of std, examples or the template that works with OpenECS's `dev`:** its pull request alone.
- **A change that spans OpenECS and the others,** such as the plugin interface or the build (the interface-change skill):
  1. OpenECS's pull request, with `std/` and `examples/` moved to the commits of their branches (the submodule-bump skill). Merge it first.
  2. Then the pull requests of std, examples and the template. Their checks now build the new `dev`.
  3. Then an OpenECS pull request that moves `std/` and `examples/` to their `dev`.
- **A branch that builds on an unmerged one:** once the first merges, merge `dev` into it, with a merge commit, and push.

## 2. Each pull request

1. Verify first (the verify skill), push the branch, then open the pull request into `dev`. The description says what changed and why, and how it was verified. Name the related pull requests as `owner/repo#number`.
2. The checks take 10 to 15 minutes; Windows is the slowest. Subscribe to the pull request's activity, which wakes the session when a check ends or a review comes. Do not poll, and do not wait idle: work on the next piece on another branch meanwhile.
3. Merge when every check passed, the pull request is mergeable, and no review thread is open: a merge commit, with the expected head commit.
4. A red check is work now: read its log, reproduce it, fix it, and push.

## 3. Pitfalls

- `git submodule update` puts a submodule back on the commit OpenECS names. After checking out another commit in `std/` or `examples/`, update only their own submodules: `git -C std submodule update --init --recursive`.
- README.md and the template's GUIDE.md change only after the owner reviews the change. Prepare the exact edits on copies, show the diff, and apply the same files after the owner's answer.
- After a restart of the session, read the pull requests' states with the GitHub tools; do not trust what the conversation remembers.
