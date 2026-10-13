# OpenECS

**Docking panels, workspaces, keys, settings and sessions come for free. You write only the panels, in C or Lua.**

OpenECS (Editor Composition System, not an entity-component system) is the empty shell of an editor-style application, written in C. It provides panels that can be split, grouped into tabs, docked, maximized and moved into their own windows, and workspaces that hold them. It also provides plugins in C and Lua, keybindings, settings, saving and restoring sessions, and operating-system features such as the clipboard, drag and drop and dialogs. It runs on Linux.

OpenECS does nothing specific to any job. Plugins decide what each panel shows and does, and a preset names the plugins and the arrangement of a tool. So the same executable becomes a text editor, a paint program, a log viewer or the front end of a game engine.

## Who it is for

- **Tool builders** who need a level editor, an asset viewer, a debugger or an internal tool, and do not want to write windows, docking and settings again.
- **Game and engine developers** who want an editor front end for their engine, with SDL3 and Lua they already know.
- **C and Lua tinkerers** who like small programs they can read and change.

The [standard plugins](https://github.com/omerfuyar/OpenECS-std) give plugins user interfaces, text grids, images, sound, networking, 3D models and files, so a panel starts from more than pixels.

## Quick start

1. Download the archive from [Releases](https://github.com/omerfuyar/OpenECS/releases), unpack it, and run an example:

   ``` shell
   ./OpenECS --fresh --preset examples/1_hello/preset.lua
   ```

2. Make your own tool from [OpenECS-template](https://github.com/omerfuyar/OpenECS-template): press "Use this template" on GitHub. It holds a preset with a plugin, and a guide that goes through each part. A Lua plugin is a folder with a manifest and a script:

   ``` lua
   -- myplugin/init.lua
   local ecs = require("ecs")
   local draw = require("draw")

   ecs.panel.registerType({
     name = "hello", -- the panel type myplugin.hello
     title = "Hello",
     create = function() return {} end,
     draw = function(_, surface)
       surface:fill(0, 0, surface.width, surface.height, "background")
       draw.text(surface, draw.font("ui"), "Hello from my plugin", 16, 16, 20, "text")
     end,
   })
   ```

3. Run your tool with its preset, `./OpenECS --fresh --preset path/to/preset.lua`. Press Alt+W then N to see the panel types of the tool and open one, and Alt+W then `,` for the settings.

The [examples](https://github.com/omerfuyar/OpenECS-examples) go on from there, one part at a time.

## Status

OpenECS is young. It works and you can build on it, but until 1.0 a minor release may change the plugin interface; each release says what changed and how to update. Releases are built for Linux on x86_64; Windows and macOS are planned. Questions, ideas and the plugins you build are welcome in [Discussions](https://github.com/omerfuyar/OpenECS/discussions).

## Documents

- [OVERVIEW.md](OVERVIEW.md): what OpenECS is, its scope, and its product decisions.
- [DESIGN.md](DESIGN.md): how OpenECS is built.
- [TODO.md](TODO.md): open questions and pending work.
- [AGENTS.md](AGENTS.md): instructions for AI agents working in this repository.

## Usage

Download the archive for your system from [Releases](https://github.com/omerfuyar/OpenECS/releases), unpack it and run `OpenECS` in it. The Linux build needs glibc 2.38 or later, such as Ubuntu 24.04 or Fedora 39.

### Command line

``` text
Usage: OpenECS [OPTION...] [FILE...]

Starts the tool a preset describes, or the launcher without one. The preset's open function
opens each FILE.

Options:
  -p, --preset NAME|FILE  Start from a preset: a name from the presets folders, or a file
  -s, --session FILE      Open a saved session
  -f, --fresh             Start from the preset, not from the tool's last session
  -t, --test FILE         Run a test; Debug builds only
  -d, --definitions DIR   Write the definition files of every plugin found into DIR, and exit
  -v, --version           Print the version and exit
  -h, --help              Print this help and exit
```

Without a preset, OpenECS shows the launcher: it lists the presets and saved sessions, and Up, Down and Return or a click open one. Press Alt+W to see the core's keys; Alt+W then `,` opens the settings window, and Alt+W then N the list of panel types.

### Examples

The `examples/` folder shows how to write plugins, one part at a time: from `1_hello` to `12_sketch` the plugin interface, then the standard plugins. Its [README](https://github.com/omerfuyar/OpenECS-examples) says how to read and run them:

``` shell
./OpenECS --fresh --preset examples/1_hello/preset.lua
```

### Writing plugins

The archive holds `include/`, the plugin interface: `OpenECS.h` for plugins in C, and `ecs.lua`, which tells editors such as VS Code what the `ecs` module of Lua plugins holds. Add that folder to `workspace.library` in your plugin's `.luarc.json`. `ecs.lua` also describes manifests, presets and settings files: write `---@type ecs.Manifest` or `---@type ecs.Preset` above the file's `return` to get completion and checks, as the examples do.

Plugins offer functions to each other. In Lua, `local draw = require("draw")` gives the functions of the plugin `draw`, if your manifest depends on it. For completion and checks of those functions, write their definition files and add the folder to `workspace.library` too; a C plugin includes the header of the same name. `--definitions` writes them for every plugin it finds, or for a preset's plugins with `--preset`:

``` shell
./OpenECS --definitions definitions/
```

## Development

### Repositories

OpenECS holds the core. Two more repositories are its submodules, so a clone with its submodules builds and ships all three:

- [OpenECS-std](https://github.com/omerfuyar/OpenECS-std), in `std/`: the standard plugins, the settings window and the launcher.
- [OpenECS-examples](https://github.com/omerfuyar/OpenECS-examples), in `examples/`: the examples.

[OpenECS-template](https://github.com/omerfuyar/OpenECS-template) is the starting point for a new tool: a preset with its plugins. It is not a submodule.

### To clone the repository

``` shell
git clone --recurse-submodules https://github.com/omerfuyar/OpenECS
```

or

``` shell
git clone https://github.com/omerfuyar/OpenECS
git submodule update --init --recursive
```

### Dependent Packages

#### Linux

##### apt

Ubuntu 18.04, all available features enabled:

``` shell
sudo apt-get update
sudo apt-get install build-essential git make pkg-config cmake ninja-build gnome-desktop-testing libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libusb-1.0-0-dev
```

Ubuntu 22.04+ can also add libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev to that command line.

##### dnf

Fedora 35, all available features enabled:

``` shell
sudo dnf install gcc git-core make cmake alsa-lib-devel fribidi-devel pulseaudio-libs-devel pipewire-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel dbus-devel ibus-devel systemd-devel mesa-libGL-devel libxkbcommon-devel mesa-libGLES-devel mesa-libEGL-devel vulkan-devel wayland-devel wayland-protocols-devel libdrm-devel mesa-libgbm-devel libusb1-devel libdecor-devel pipewire-jack-audio-connection-kit-devel libthai-devel
```

Fedora 39+ can also add liburing-devel to that command line.

Fedora 40+ needs zlib-ng-compat-static to be added to that command line.

The sndio audio target is unavailable on Fedora (but probably not what you should want to use anyhow).

##### zypper

``` shell
sudo zypper in libunwind-devel libusb-1_0-devel Mesa-libGL-devel libxkbcommon-devel libdrm-devel libgbm-devel pipewire-devel libpulse-devel sndio-devel Mesa-libEGL-devel alsa-devel xwayland-devel wayland-devel wayland-protocols-devel libthai-devel fribidi-devel libXi-devel libXcursor-devel libXi-devel libX11-devel libXext-devel libXfixes-devel libXrandr-devel libXrender-devel libXss-devel libXtst-devel
```

##### pacman

``` shell
sudo pacman -S alsa-lib cmake hidapi ibus jack libdecor libthai fribidi libgl libpulse libusb libx11 libxcursor libxext libxfixes libxi libxinerama libxkbcommon libxrandr libxrender libxss libxtst mesa ninja pipewire sndio vulkan-driver vulkan-headers wayland wayland-protocols
```

### Building

The code is C23, so it needs a C23 compiler such as gcc 14 or later. SDL and SDL_ttf also need cmake and ninja.

Shuild builds the program. Compile the build script once; it compiles itself again when `shuild.c` changes:

``` shell
cd OpenECS/
gcc shuild.c -o shuild.ignore -O3 -Wno-format-truncation
./shuild.ignore -b release -e
```

``` text
Usage: ./shuild.ignore [FLAG...]

Flags:
  -b, --build TYPE   Build type: debug (the default), with the static analyzer and the sanitizers;
                     release; relwithdebinfo, a release with debug information; or minsizerel, a small release
  -n, --no-std       Do not build the standard plugins of std/
  -e, --examples     Also build the examples of examples/, into bin/examples/
  -h, --help         Show this help
```

The build puts the executable, SDL's shared libraries, the standard plugins, the first-party presets and the examples in `build/<TYPE>/bin/`, such as `build/Release/bin/`. SDL and SDL_ttf are shared libraries; Lua, Clay, libffi and stb are linked into the executable.

The standard plugins and the examples are in their own repositories, checked out in `std/` and `examples/`. Each has its own `shuild.c`, which this build compiles into `shuild.ignore` in that folder and runs, so they build with the same type into the same folder. `-n` leaves the standard plugins out, and `-e` adds the examples.

Dependencies are built the first time only. To build one again, delete its library from `build/<TYPE>/lib/` and the `.shu/` folder; shuild does not make a library again while its compiled files are unchanged.

Shuild compiles again only the files that changed. After changing compiler flags in `shuild.c`, delete `.shu/` (or run `sudo git clean -Xfd` to delete all ignored files) to compile everything again.

The build also writes the definition files of the plugins it built into `build/<TYPE>/definitions/`, and a `.luarc.json` in this folder, `std/` and `examples/` that names them with `include/`. So an editor with LuaLS, such as VS Code with the Lua extension, completes and checks the `ecs` module and `require("ui")` in every Lua file of the three repositories.

Debug builds run the static analyzer while compiling, and the sanitizers while the program runs. A sanitizer prints its report to standard error, and the program exits with an error.

### Testing

The examples are the tests. Each example has a test, which Debug builds run with `--test`. The build copies the examples into `bin/examples/`, so build again after changing one. A test needs no display, and prints "The test passed." or the reason it failed. These commands build with the examples and run every test, and name the ones that fail:

``` shell
./shuild.ignore -b debug -e
.github/scripts/test.sh build/Debug/bin/OpenECS
```

To see why a test fails, run it alone: `./build/Debug/bin/OpenECS --test build/Debug/bin/examples/3_ui/test.lua`. DESIGN.md section 17.5 explains how to write one.

### Checks

GitHub checks every pull request (DESIGN.md section 19.3). These commands run the same checks on your computer:

``` shell
.github/scripts/build.sh D
.github/scripts/test.sh build/Debug/bin/OpenECS
.github/scripts/build.sh R
.github/scripts/definitions.sh build/Debug/bin/OpenECS
```

## License

OpenECS is under the zlib license; see [LICENSE.md](LICENSE.md). The release archives hold the licenses of the libraries and the font that OpenECS includes, in `licenses/`.
