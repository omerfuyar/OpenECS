# AGENTS.md

Instructions for AI agents working in this repository. Read this file fully before doing anything.

This is the only file written for agents. Everything else is written for humans. This file does not repeat project information; it tells you where to find it.

## Where things are

| File | What it holds |
|---|---|
| [README.md](README.md) | Short introduction, cloning and building. |
| [OVERVIEW.md](OVERVIEW.md) | What OpenECS is: scope, the line between core and plugins, principles and product decisions. |
| [DESIGN.md](DESIGN.md) | How OpenECS is built: modules, interfaces, conventions, dependencies and rules for contributors. |
| [TODO.md](TODO.md) | Open questions, proposals waiting for the owner, and pending tasks. |

Read OVERVIEW.md and DESIGN.md before you propose or change anything.

| Path | What it holds |
|---|---|
| `src/`, `include/` | The core's source code and public headers. |
| `dependencies/` | Third-party git submodules. |
| `resources/` | Files the program loads at run time. |
| `shuild.c` | The build script (see README.md). |

## Current stage

The project is being designed. Work on the documents. Do not write the implementation unless the owner asks.

## How to work with the owner

1. **Apply good solutions, then inform.** When research gives you a working, well-supported solution, write it into the documents as decided and tell the owner what you decided and why. Do not ask first.
2. **Ask when it is really unsure,** or when it is a matter of the owner's taste. Then explain the options and trade-offs, recommend one, and raise one decision at a time. Do not assume; the project must not be misunderstood.
3. **Research before proposing.** Check the facts in documentation, source code or on the web, and show the evidence.
4. **Design first.** Focus on the design of the product, not on implementation, unless asked otherwise.
5. **Correct the owner** when an idea would cause problems, and say why. Do not follow any idea blindly, including the owner's.
6. **Do not be afraid** to delete things, to propose the opposite, or to change the design when it makes sense.
7. **Examples are ideas, not constraints.** Think widely and long term.
8. **Keep the documents consistent.** When a decision is made, update OVERVIEW.md, DESIGN.md and TODO.md together.

## Boundaries

**Always**

- Write text and code for a human reader: simple, short and easy to read. Do not bloat.
- Mark clearly what is not decided (see "Writing documents").

**Ask first**

- Changing anything that is marked as decided.
- Adding a dependency.
- Changing code, build files or submodules.

**Never**

- Commit or push, unless the owner tells you to.
- Stage changes (`git add`). Leave them unstaged, so the owner sees them in the editor.
- Edit anything under `dependencies/`.
- Invent your own code style. Follow DESIGN.md, section 1, which comes from the owner's own projects. Commit messages follow DESIGN.md, section 18.3.

## Writing documents

- Markdown only.
- Explain every term where it first appears, and add it to that document's glossary.
- No roadmaps, phases or schedules.
- Product decisions go in OVERVIEW.md, technical decisions in DESIGN.md, and everything undecided or pending in TODO.md.
- Everything written in OVERVIEW.md and DESIGN.md is decided. Mark only what still waits for the owner: **Proposed** in OVERVIEW.md, **[Proposed]** in DESIGN.md, and list it in TODO.md.
- Use short sentences and plain words:
  - Good: "A group with one panel shows no tab row."
  - Bad: "It should be noted that, in cases where a group contains only a single panel, the tab row is not displayed."
