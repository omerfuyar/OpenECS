# AGENTS.md

Instructions for AI agents working in this repository. Read this file fully before doing anything.

This is the only file written for agents. Everything else is written for humans. This file does not repeat project information; it tells you where to find it.

## Where things are

| File                       | What it holds                                                                                    |
| -------------------------- | ------------------------------------------------------------------------------------------------ |
| [README.md](README.md)     | Short introduction, cloning and building.                                                        |
| [OVERVIEW.md](OVERVIEW.md) | What OpenECS is: scope, the line between core and plugins, principles and product decisions.     |
| [DESIGN.md](DESIGN.md)     | How OpenECS is built: modules, interfaces, conventions, dependencies and rules for contributors. |
| [TODO.md](TODO.md)         | Open questions and pending tasks.                                                                |

Read OVERVIEW.md and DESIGN.md before you propose or change anything.

| Path            | What it holds                                                                                                          |
| --------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `include/`      | Headers: the plugin header and the core's module headers, and `ecs.lua` for editors (DESIGN.md, sections 1.5 and 9.6). |
| `src/`          | The core's source files.                                                                                               |
| `plugins/`      | First-party plugins.                                                                                                   |
| `presets/`      | First-party presets.                                                                                                   |
| `dependencies/` | Third-party git submodules.                                                                                            |
| `resources/`    | Files the program loads at run time.                                                                                   |
| `tests/`        | Tests that Debug builds run (DESIGN.md, section 17.5).                                                                 |
| `shuild.c`      | The build script (see README.md).                                                                                      |

## Current stage

The implementation has started. Build it in small milestones, and keep DESIGN.md in step with the code.

## How to work with the owner

1. **Apply good solutions, then inform.** When research gives you a working, well-supported solution, write it into the documents as decided and tell the owner what you decided and why. Do not ask first.
2. **Ask when it is really unsure,** or when it is a matter of the owner's taste. Then explain the options and trade-offs, recommend one, and raise one decision at a time. Do not assume; the project must not be misunderstood.
3. **Research before proposing.** Check the facts in documentation, source code or on the web. Show the evidence in your reply, not in the documents.
4. **Design first.** Focus on the design of the product, not on implementation, unless asked otherwise.
5. **Correct the owner** when an idea would cause problems, and say why. Do not follow any idea blindly, including the owner's.
6. **Do not be afraid** to delete things, to propose the opposite, or to change the design when it makes sense.
7. **Examples are ideas, not constraints.** Think widely and long term.
8. **Keep one source of truth.** When a decision is made, write it in the one place it belongs and remove it from TODO.md.
9. **Recommend libraries.** When a trusted, widely used library would make development or maintenance easier, for example for strings or containers, recommend it. Check SDL first; it may already have what is needed.

## Boundaries

**Always**

- Write text and code for a human reader: simple, short and easy to read. Do not bloat.

**Ask first**

- Changing anything that is marked as decided.
- Adding a dependency.
- Changing submodules.

**Never**

- Push, unless the owner tells you to.
- Edit anything under `dependencies/`.
- Break or invent code conventions. Follow DESIGN.md section 1.

## Git

- Commit each finished piece of work. Do not leave changes uncommitted.
- Work on a branch: `feature/<feature>`, `fix/<bug>`, `refactor/<area>` or `docs/<area>`. Name it after the change; never use a generated name.
- Create the branch from `main` and publish it at the start of the work, then push each commit to it.
- Keep branches separate. Do not start a branch from another branch that is not merged yet.
- Commit messages have one line per change, starting with `-`. Details go on lines starting with `--`.

## Writing documents

- Markdown only.
- One source of truth: write each thing in one place only. Elsewhere, link to it instead of repeating it.
- Put each thing in its place: product decisions in OVERVIEW.md, technical decisions in DESIGN.md, open questions and pending work in TODO.md, introduction and building in README.md, instructions for agents in this file.
- OVERVIEW.md and DESIGN.md explain the design and nothing else: no history, sources, research evidence, comparisons with alternatives, status or open questions. Everything in them is decided.
- Explain terms only in the glossary of the document that uses them, never inline.
- Do not invent framings, categories or rules, such as "three rules shape the architecture". Do not make vague claims, such as "an ordinary screen".
- Keep section and list numbers in order, and update cross-references when they change.
- No roadmaps, phases or schedules.
- Use short sentences and plain words:
  - Good: "A group with one panel shows no tab row."
  - Bad: "It should be noted that, in cases where a group contains only a single panel, the tab row is not displayed."
