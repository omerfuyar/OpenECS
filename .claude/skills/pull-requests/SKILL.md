---
name: pull-requests
description: Open, follow and merge the pull requests of a change across OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template, in the order their checks need. Use after verifying a change, and when opening, merging or following its pull requests.
---

# Pull requests across the repositories

CLAUDE.md, section Git, holds the rules: branches from `dev`, merge commits, never `main`. This is the order that makes every check pass, and how to wait for them cheaply.

## 1. The order

OpenECS's checks build `std/` and `examples/` at the commits its submodules name. The checks of the other three build OpenECS's `dev`, or, on a pull request, OpenECS's branch of the same name when there is one. std's checks also take the examples' branch of the same name, and the examples' checks std's (DESIGN.md, section 17.7). So:

- **A change in one of std, examples or the template that works with OpenECS's `dev`:** its pull request alone.
- **A change that spans the repositories,** such as the plugin interface or the build (the interface-change skill):
  1. Give its branch the same name in each repository it touches, and push each.
  2. OpenECS's branch moves `std/` and `examples/` to the heads of their branches (the submodule-bump skill), even when it changes nothing else. When a branch gets new commits, move the submodule to its new head too.
  3. Open every pull request at once. Their checks run together.
  4. Merge std's and the examples' first, then OpenECS's, then the template's. A merge commit keeps each branch's head in its `dev`, so OpenECS's submodules need no second pull request.
- **A branch that builds on an unmerged one:** once the first merges, merge `dev` into it, with a merge commit, and push.

## 2. Each pull request

1. Verify first (the verify skill), push the branch, then open the pull request into `dev`. The description says what changed and why, and how it was verified. Name the related pull requests as `owner/repo#number`.
2. The checks take 10 to 15 minutes; Windows is the slowest. A change of documents alone skips the builds and tests. Subscribe to the pull request's activity, which wakes the session when a check ends or a review comes. Do not poll, and do not wait idle: work on the next piece on another branch meanwhile.
3. Merge when every check passed, the pull request is mergeable, and no review thread is open: a merge commit, with the expected head commit.
4. A red check is work now: read its log, reproduce it, fix it, and push.

## 3. Pitfalls

- `git submodule update` puts a submodule back on the commit OpenECS names. After checking out another commit in `std/` or `examples/`, update only their own submodules: `git -C std submodule update --init --recursive`.
- README.md and the template's GUIDE.md change only after the owner reviews the change. Prepare the exact edits on copies, show the diff, and apply the same files after the owner's answer.
- After a restart of the session, read the pull requests' states with the GitHub tools; do not trust what the conversation remembers.
