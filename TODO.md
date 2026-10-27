# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **GLSL shaders on Direct3D 12.** gpu compiles GLSL to SPIR-V, and for Direct3D 12 turns it into DXBC through SPIRV-Cross's HLSL and Windows' d3dcompiler_47.dll (OpenECS-std DESIGN 15). The checks run no GPU on Windows; to confirm with 22_gpu_model on Windows.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **Import the rulesets.** The rulesets of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template require the Windows checks now, as the owner chose. The owner imports them in each repository's settings. OpenECS-examples' rulesets no longer require `Release (Linux)`, which it does not run.

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the build tool, a single header at `dependencies/shuild`. Shu, at `dependencies/shu`, holds the types and macros it uses. The owner allows changes to both, in their style, through pull requests into their main branches. These repositories have no CI, so a pull request is merged with a merge commit once it is tested locally: shuild's examples, and OpenECS built from scratch with its checks. Then an OpenECS pull request moves the submodule. Work found in it:

- **Windows and macOS.** Shuild names libraries for the system its compiler compiles for, and cross-compiles for Windows with MinGW (omerfuyar/shuild#23). Its Windows host code runs in OpenECS's Windows checks, which build OpenECS on Windows. `SHU_UtilRun` runs commands with the system's shell, which is `cmd.exe` on Windows.
- **Its own todo.** The header lists a module macro, like Clay's. The owner leaves it for later.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **web.** HTTP/2 and compressed answers, which need more libraries or more of libcurl.
- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.
- **A parser plugin.** Reading and writing common text formats, such as `.env` and JSON files, into values plugins use. Which formats, and which library, are open.

## Questions

- **Vectors in cglm.** cglm's Lua functions pass vectors and matrices as strings of packed floats. Decided: Lua gets tables with `x`, `y`, `z` and `w` that it may change, with operators; a matrix is 16 numbers, column by column; a vector times a vector is an error, and `dot` and `cross` are methods; the types belong to the cglm plugin, and cglm's own headers stay inside it. Open: how a plugin's value type, such as `struct<cglm.vec3>`, gets its operators and crosses signatures. A generic core feature like handle types (DESIGN 10.6) was proposed; the owner thinks handles may already cover it. The owner decides it later.
- **Desktop entries.** A preset gives the tool's identity (DESIGN 13.2). It could also describe the tool's entry in the system's application launcher: its categories, and the file types its `open` function takes, so installing a tool adds it to the launcher and to "Open with". To decide: whether presets describe it, and whether OpenECS or an installer writes the `.desktop` file. The owner decides it later.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **GIFs of what OpenECS can do** in README.md, which the owner records.
- **GitHub settings**, which the owner sets for each of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template:
  - a description, one line on what the repository holds;
  - topics, such as `sdl3`, `lua`, `editor`, `plugin-system`, `c23` and `tooling`;
  - a social preview image, such as a screenshot of a tool with its panels;
  - issues labelled "good first issue".

Then announce in this order, each post written for its community: SDL's forum and the Handmade Network; r/C_Programming, r/lua, r/gamedev, r/linux and r/opensource; Show HN and Lobsters; the Lua mailing list. After a release, a short devlog, and a monthly "how I built X with OpenECS" post.

What would grow it most:

- **Releases for macOS**, since most tool builders are not on Linux. Windows has one since 0.4.0 (see Tasks).
- **A flagship tool** that is useful without writing code, such as a log viewer, a glTF viewer or a terminal, built from the standard plugins.
- **A showcase** in README.md of plugins and tools others build.
- **One place to talk**, Discord or Matrix, linked from README.md.

