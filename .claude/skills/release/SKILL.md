---
name: release
description: Prepare a release of OpenECS, OpenECS-std and OpenECS-examples together. Use when the owner asks for a release, a version, release notes or pull requests into main. The agent writes the descriptions and opens the pull requests; the owner merges into main and pushes the tags.
---

# Prepare a release

DESIGN.md, section 19, holds the rules: 19.1 versions, 19.4 the steps, 19.5 how a description is written. Read them first. Never merge into `main` and never push a tag; the owner does both.

## 1. Choose the version

- Read `OPENECS_VERSION` in `include/OpenECS.h`: between releases it is the next version with `-dev`.
- Check it against 19.1 with `git log --oneline v<last>..origin/dev` in each repository: anything that breaks plugins, presets, settings or sessions makes it a minor release before 1.0.0.
- Check that `OPENECS_API_VERSION` and the `format` of presets and sessions changed if they had to.

## 2. OpenECS-std and OpenECS-examples

In each, on a `docs/release-<version>` branch from `dev`:

1. Write `.github/release-notes/v<version>.md` as 19.5 says, about that repository's changes only. Read the last description for its form.
2. Open a pull request into `dev`, and when it is merged, one from `dev` into `main`.

## 3. OpenECS

On a `docs/release-<version>` branch from `dev`:

1. Move `std/` and `examples/` to the `main` commits of step 2 (the submodule-bump skill).
2. Set `OPENECS_VERSION` to `<version>`, without `-dev`.
3. Write `.github/release-notes/v<version>.md`; it links to the other two releases.
4. Verify (the verify skill), push, and open the pull request into `dev`; when it is merged, one from `dev` into `main`.

## 4. Hand over to the owner

Give the owner the commands, OpenECS's tag first, because the others' release workflows check out OpenECS at the same tag:

```sh
git -C OpenECS tag v<version> <merge commit on main> && git -C OpenECS push origin v<version>
git -C OpenECS-std tag v<version> <merge commit on main> && git -C OpenECS-std push origin v<version>
git -C OpenECS-examples tag v<version> <merge commit on main> && git -C OpenECS-examples push origin v<version>
```

The owner checks the three draft releases and publishes them. Then, on OpenECS's `dev`, set `OPENECS_VERSION` to the next version with `-dev` in a pull request.
