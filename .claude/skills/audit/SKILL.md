---
name: audit
description: Audit OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template for differences between what their documents say and what their code does, and record the findings in TODO.md. Use when asked to audit the documents, check that the docs match the code, or find what the documents miss or get wrong. Audit only; it changes no code.
---

# Audit the documents against the code

An audit finds where a document and the code differ, and records each difference for the owner. It changes no code. The [review](../review/SKILL.md) skill finds what to improve in the code; this one finds what is said wrongly or not at all.

## 1. Read first

- CLAUDE.md, the Contents and section 0 of DESIGN.md, and the Contents of std's DESIGN.md. Then each section in full before you audit its area.
- TODO.md, so no finding repeats an item it holds.
- What DESIGN.md decides is not a gap, even if you would design it otherwise, such as std DESIGN 5's "a click elsewhere drops what was typed". Keep such a case apart, as a design question.

## 2. What a finding is

One of:

1. **A document says X, the code does Y.**
2. **The code does X that no document says:** a setting, key, function, event, field, test function or misuse message.
3. **A document says X, and nothing does it**, or it exists under another name.
4. **Documents disagree:** DESIGN, GUIDE, README, std's DESIGN, the comments of ecs.lua, OpenECS.h, `resources/settings.lua`, the manifests and the examples.

Each needs evidence from both sides: the document's line (file:line, quoted in under 15 words) and the code's line, or a test run that shows the behaviour. Read the code path to the end, grep every spelling, and check the C and the Lua side. Drop what you cannot confirm, or list it as unconfirmed.

## 3. The areas

| Area | Check |
| --- | --- |
| The interface | `include/OpenECS.h` against `include/ecs.lua`: each function, its `@lua` line, parameters, results and comment (DESIGN 11.5). Both against DESIGN's tables (7.5, 8.4, 10.6, 11.3, 12, 17.5), and against what `src/` registers: the core's functions (`OPENECS_CORE_FUNCTIONS` in Menus.c, and those registered in App.c and Session.c), settings declared in each module, events emitted, and the test functions in Test.c. |
| The core's behaviour | DESIGN 4 to 9 and 13 against `src/interface/`, `src/runtime/` and `src/app/Session.c`: layout, tabs, grips, menus, the title bar, the prefix, focus, drag and drop, dialogs, popups, workspaces, sessions and presets. |
| GUIDE.md | Each key, menu entry and step against `resources/settings.lua` and the menus' code. |
| std | Each section of std's DESIGN.md against its plugin: names, props, callbacks, defaults, colours, keys, menu entries, and its manifest's description. |
| Examples | Each example's comments against its code and test (DESIGN 17.6); its test checks what its first comment promises; its preset uses no removed setting; the examples' README and CLAUDE.md list them as they are. |
| The template | `../OpenECS-template`: which OpenECS it builds against, the `api` of its manifests against `OPENECS_API_VERSION`, whether its plugins use the current interface and std's idioms (compare std's panels and the newest examples), whether its preset, test and GUIDE match OpenECS's GUIDE, keys, menus and title bar, and whether its CLAUDE.md follows OpenECS's. |

## 4. Split the work

- Give each area to a read-only `Explore` agent in the background, with its row above, sections 1 and 2 of this skill, and the items TODO.md already holds. Ask for at most 15 findings, each with both lines and the side that should change, then its design questions and its unconfirmed items.
- An agent stopped by a usage limit keeps its work: send it a message to continue, rather than starting a new one.
- Meanwhile, build and run the tests (section 5).
- Check each finding yourself at the lines it cites, by grepping its text, not its line number; drop what does not hold.

## 5. Runs

Offscreen only: never open a window on the owner's screen.

- Build with `.github/scripts/build.sh D` and run `.github/scripts/test.sh`, in the background (the verify skill).
- shuild writes every test's output to one `test.log`, so to read each test's warnings, run each test alone into a log of its own: `SDL_VIDEO_DRIVER=offscreen build/Debug/bin/OpenECS --test <test>`, for every `build/Debug/bin/examples/*/test*.lua`. Then grep the logs for `not declared`, `not a declared setting` and other warnings and errors; most errors are a test's own, on purpose.
- To settle a behaviour, write a test in the scratchpad whose `preset` is the absolute path of an example's preset in the build, and use `test.screenshot`, `test.titleBar`, `test.popup` and `test.session`.
- The template's test: copy the template's `dev` (`git archive origin/dev`) and its `dependencies/` to the scratchpad, lay out OpenECS's `build/Debug/bin/` in its `OpenECS/` as its `checks.yml` does (`OpenECS --definitions OpenECS/include`), build with its shuild, and run `OpenECS/OpenECS --test test.lua`.

## 6. Record

- Work in a worktree from `dev` (`git worktree add -b docs/<name> ../OpenECS-<name> origin/dev`): other sessions may move OpenECS's checkout while the audit runs. Before writing, check what `dev` gained since, and grep each cited text again for its current line.
- Write each finding in TODO.md, in its style: a bold title, then what differs, with file:line on both sides, and which side should change and why. The core's and GUIDE's go in Tasks, std's in Standard plugins, the examples' and the template's in sections of their own. Paths in an item are of the repository the section names.
- Do not edit a README.md or the template's GUIDE.md: write the exact change you propose in the reply.
- Commit as CLAUDE.md's Git section says, open the pull request into `dev` and turn on auto-merge with a merge commit.

## 7. Reply

- The count of findings per area, the five that matter most, and the unconfirmed list.
- The proposed changes to READMEs and the template's GUIDE.
- The design questions, in one batch, each with its options and a recommendation.
