# OpenECS

OpenECS (Editor Composition System) is the empty shell of an editor-style application, written in C. It provides panels that can be split, grouped into tabs, docked, maximized and moved into their own windows, and workspaces that hold them. It also provides plugins in C and Lua, keybindings, settings, saving and restoring sessions, and operating-system features such as the clipboard, drag and drop and dialogs. It runs on Linux.

OpenECS does nothing specific to any job. Plugins decide what each panel shows and does, and a preset names the plugins and the arrangement of a tool. So the same executable becomes a text editor, a paint program or the front end of a game engine. OpenECS is an executable, not a library.

## Documents

- [OVERVIEW.md](OVERVIEW.md): what OpenECS is, its scope, and its product decisions.
- [DESIGN.md](DESIGN.md): how OpenECS is built.
- [TODO.md](TODO.md): open questions and pending work.
- [AGENTS.md](AGENTS.md): instructions for AI agents working in this repository.

## Usage

Download the archive for your system from [Releases](https://github.com/omerfuyar/OpenECS/releases), unpack it and run `OpenECS` in it. The Linux build needs glibc 2.38 or later, such as Ubuntu 24.04 or Fedora 39. `OpenECS --version` prints its version.

The archive also holds `include/`, the plugin interface: `OpenECS.h` for plugins in C, and `ecs.lua`, which tells editors such as VS Code what the `ecs` module of Lua plugins holds. Add that folder to `workspace.library` in your plugin's `.luarc.json`.

## Development

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

Shuild builds the program:

Usage:
./shuild [TYPE [LINK]]

Arguments:
TYPE
    D   Debug (Default)
    R   Release
    RD  RelWithDebInfo
    SR  MinSizeRel
LINK
    S   Static (Default)
    D   Dynamic

So this command will build the program mode statically linked release mode.

``` shell
cd OpenECS/
gcc shuild.c -o shuild.ignore -O3
./shuild.ignore R S
```

Dependencies are built the first time only. To build one again, delete its library from `build/<LINK>/<TYPE>/lib/` and the `.shu/` folder; shuild does not make a library again while its compiled files are unchanged.

Shuild compiles again only the files that changed. After changing compiler flags in `shuild.c`, delete `.shu/` to compile everything again.

Debug builds run the static analyzer while compiling, and the sanitizers while the program runs. A sanitizer prints its report to standard error, and the program exits with an error.

### Running

The build puts the executable, the first-party plugins and presets in `build/<LINK>/<TYPE>/bin/`.

``` shell
./build/Static/Release/bin/OpenECS
./build/Static/Release/bin/OpenECS --preset path/to/preset.lua
```

Press Alt+W to see the core's keys.

The default preset shows two example plugins that do the same things, `sketch_c` in C and `sketch_lua` in Lua, so their code can be compared: workspace 1 holds the C canvas, workspace 2 the Lua canvas, and workspace 3 both. Draw with the mouse; the wheel changes the brush size. On a canvas, Ctrl+C and Ctrl+V copy and paste strokes, also between the two plugins, Ctrl+B opens a canvas beside, Ctrl+G gathers every canvas, Ctrl+E exports an image and Delete clears. Ctrl+Tab switches workspace.

### Testing

Debug builds run the tests in `tests/`. A test needs no display, and prints "The test passed." or the reason it failed. This command runs them all and names the ones that fail:

``` shell
for test in tests/*.lua; do ./build/Static/Debug/bin/OpenECS --test "$test" > /dev/null 2>&1 || echo "failed: $test"; done
```

To see why a test fails, run it alone. DESIGN.md section 17.5 explains how to write one.

### Checks

GitHub checks every pull request (DESIGN.md section 19.3). These commands run the same checks on your computer:

``` shell
.github/scripts/build.sh D
.github/scripts/test.sh build/Static/Debug/bin/OpenECS
.github/scripts/build.sh R
```

## License

OpenECS is under the zlib license; see [LICENSE](LICENSE). The release archives hold the licenses of the libraries and the font that OpenECS includes, in `licenses/`.
