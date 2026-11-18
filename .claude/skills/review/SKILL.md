---
name: review
description: Review an area of OpenECS or OpenECS-std and record improvement recommendations in TODO.md. Use when asked to review, audit or find improvements in the code or design, such as the core's services and events or the ui plugin. Review only; it changes no code.
---

# Review an area

A review finds what to improve and records it for the owner. It changes no code. [areas.md](areas.md) names each area's files, its DESIGN.md sections and the questions to ask it.

## 1. Read only what the area needs

- CLAUDE.md, DESIGN.md section 1 and OpenECS-std's DESIGN.md section 1, once.
- The area's sections in areas.md, then its DESIGN.md sections. Not the whole documents.
- Source by function, not whole files: `grep -n` for the function or the struct, then read that range. The large files (Services.c, Window.c, Bindings.c, Layout.c, ui's `init.lua`) never need reading whole.
- Never `dependencies/`. Read a dependency's header only to check how the code uses it.
- No build and no tests, unless a finding needs proof, such as a claim about speed or a crash. Then use the verify skill, and name the proof in the finding.

## 2. Split the work

For more than one area, give each area to a read-only `Explore` agent with its section of areas.md and the checklist and format below, so each agent's reading stays out of this session. Ask each for at most 10 findings and no quoted code beyond the lines a finding needs. Then check each finding yourself at the lines it cites; drop what does not hold.

## 3. What to look for

In this order, as it matters most:

1. **Correctness.** Ownership and lifetimes (DESIGN.md 15), error paths that leak or leave half a state, calls back into the core from a callback, a plugin disabled and enabled again (9.10), threads (3.3).
2. **The design.** Where the code and DESIGN.md differ, where an interface is hard to use well or easy to use wrongly, where C and Lua differ (11.5), names that break section 1.2.
3. **Speed.** Work done each frame or each event that need not be: allocations, linear searches, strings compared or copied, whole redraws.
4. **Simplicity.** Code repeated, code no one calls, functions that do several things, parts that a library or SDL already provides (CLAUDE.md, "Recommend libraries").
5. **Tests.** Behaviour no example's test covers (DESIGN.md 17.5).

Skip what clang-tidy, clang-format and gcc's warnings already check.

## 4. Write the findings

Each finding is one item:

```md
- **P1 · Short title.** `src/runtime/Events.c:120` What is wrong and what it causes. The recommendation. Size: S, M or L.
```

- P1: a bug, or a design flaw that gets harder to fix with each plugin. P2: worth doing before 1.0.0. P3: worth doing when the code is touched.
- One problem per item. Name the lines, so the owner can check it. No praise and no summaries of what the code does.
- A recommendation that changes the plugin interface says so; it follows the interface-change skill when it is done.

Write them in TODO.md, in a section `## Review`, under one bold line per area, P1 first. TODO.md's rules apply: when the owner decides an item, its decision goes into the design and the item leaves TODO.md. Commit it as CLAUDE.md's Git section says.

In the reply, give the P1 items and the questions that need the owner's decision, in one batch.
