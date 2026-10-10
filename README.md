# OpenECS

OpenECS (Editor Composition System) is the empty shell of an editor-style application, written in C. It provides panels that can be split, grouped into tabs, docked, maximized and moved into their own windows, and workspaces that hold them. It also provides plugins in C and Lua, keybindings, settings, saving and restoring sessions, and operating-system features such as the clipboard, drag and drop and dialogs. It runs on Linux.

OpenECS does nothing specific to any job. Plugins decide what each panel shows and does, and a preset names the plugins and the arrangement of a tool. So the same executable becomes a text editor, a paint program or the front end of a game engine. OpenECS is an executable, not a library.

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
  -d, --definitions DIR   Write the definition files of the plugins' functions into DIR, and exit
  -v, --version           Print the version and exit
  -h, --help              Print this help and exit
```

Without a preset, OpenECS shows the launcher: it lists the presets and saved sessions, and Up, Down and Return or a click open one. Press Alt+W to see the core's keys; Alt+W then `,` opens the settings window.

### Examples

The `examples/` folder shows how to write plugins, one part at a time, from `1_hello` to `12_sketch`. Its [README](https://github.com/omerfuyar/OpenECS-examples) says how to read and run them:

``` shell
./OpenECS --fresh --preset examples/1_hello/preset.lua
```

### Writing plugins

The archive holds `include/`, the plugin interface: `OpenECS.h` for plugins in C, and `ecs.lua`, which tells editors such as VS Code what the `ecs` module of Lua plugins holds. Add that folder to `workspace.library` in your plugin's `.luarc.json`. `ecs.lua` also describes manifests, presets and settings files: write `---@type ecs.Manifest` or `---@type ecs.Preset` above the file's `return` to get completion and checks, as the examples do.

Plugins offer functions to each other. In Lua, `local draw = require("draw")` gives the functions of the plugin `draw`, if your manifest depends on it. For completion and checks of those functions, write their definition files and add the folder to `workspace.library` too; a C plugin includes the header of the same name:

``` shell
./OpenECS --preset paint --definitions definitions/
```

## Development

### Repositories

OpenECS holds the core. Two more repositories are its submodules, so a clone with its submodules builds and ships all three:

- [OpenECS-std](https://github.com/omerfuyar/OpenECS-std), in `std/`: the standard plugins, the settings window and the launcher.
- [OpenECS-examples](https://github.com/omerfuyar/OpenECS-examples), in `examples/`: the examples.

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

#### Windows

todo winget, llvm or mingw, autoreconf etc.

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
gcc shuild.c -o shuild.ignore -O3
./shuild.ignore -b release -e
```

``` text
Usage: ./shuild.ignore [FLAG...]

Flags:
  -b, --build TYPE   Build type: debug (the default), with the static analyzer and the sanitizers;
                     release; relwithdebinfo, a release with debug information; or minsizerel, a small release
  -n, --no-std       Do not build the standard plugins of std/
  -e, --examples     Also build the examples of examples/, into bin/examples/
  -t, --tests        Also build the tests, into tests/ beside bin/, and std's into std/tests/ beside bin/
  -h, --help         Show this help
```

The build puts the executable, SDL's shared libraries, the standard plugins, the first-party presets and the examples in `build/<TYPE>/bin/`, such as `build/Release/bin/`. SDL and SDL_ttf are shared libraries; Lua, Clay, libffi and stb are linked into the executable.

The standard plugins and the examples are in their own repositories, checked out in `std/` and `examples/`. Each has its own `shuild.c`, which this build compiles into `shuild.ignore` in that folder and runs, so they build with the same type into the same folder. `-n` leaves the standard plugins out, and `-e` adds the examples.

Dependencies are built the first time only. To build one again, delete its library from `build/<TYPE>/lib/` and the `.shu/` folder; shuild does not make a library again while its compiled files are unchanged.

Shuild compiles again only the files that changed. After changing compiler flags in `shuild.c`, delete `.shu/` (or run `sudo git clean -Xfd` to delete all ignored files) to compile everything again.

Debug builds run the static analyzer while compiling, and the sanitizers while the program runs. A sanitizer prints its report to standard error, and the program exits with an error.

### Testing

Debug builds built with `-t` run the tests. The build copies `tests/` beside `bin/`, so build again after changing a test. A test needs no display, and prints "The test passed." or the reason it failed. Each repository tests what it holds: these commands run OpenECS's tests, then the standard plugins' tests, then the examples' tests, and name the ones that fail:

``` shell
./shuild.ignore -b debug -e -t
.github/scripts/test.sh build/Debug/bin/OpenECS
.github/scripts/test.sh build/Debug/bin/OpenECS std
.github/scripts/test.sh build/Debug/bin/OpenECS examples
```

To see why a test fails, run it alone: `./build/Debug/bin/OpenECS --test build/Debug/tests/menus.lua`. DESIGN.md section 17.5 explains how to write one.

### Checks

GitHub checks every pull request (DESIGN.md section 19.3). These commands run the same checks on your computer:

``` shell
.github/scripts/build.sh D
.github/scripts/test.sh build/Debug/bin/OpenECS
.github/scripts/build.sh R
```

## License

OpenECS is under the zlib license; see [LICENSE.md](LICENSE.md). The release archives hold the licenses of the libraries and the font that OpenECS includes, in `licenses/`.
