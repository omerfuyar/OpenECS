---
name: submodule-bump
description: Move OpenECS's std/ and examples/ submodules to a newer commit of their repository. Use after a pull request into OpenECS-std's or OpenECS-examples' dev is merged, when OpenECS needs a change made there, or for a release.
---

# Move the submodules

A submodule names one commit of its repository's `dev`, or of `main` for a release; OpenECS moves it forward in a pull request (DESIGN.md, section 17.7). Never change any other submodule without asking the owner.

1. On an OpenECS branch from `dev`, move each submodule that changed:

   ```sh
   git -C std fetch origin dev && git -C std checkout --detach origin/dev
   git -C examples fetch origin dev && git -C examples checkout --detach origin/dev
   git submodule update --init --recursive std examples
   ```

   For a branch that is not merged yet, check out that branch's commit instead, and move the submodule again to `dev` once it is merged.

2. Verify with the verify skill: the new commits must build and pass the tests with OpenECS.
3. Commit with one line per moved submodule that says what it brings, such as `- std/ moves to OpenECS-std's dev: the panels plugin`.
4. Push and open the pull request into OpenECS's `dev`.
