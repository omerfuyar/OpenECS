# OpenECS
This is a ECS (Entity Component System) based game engine project with complete core and editor.

## Usage
Just download the correct build for your setup from releases and run the executable.

## Development

### To clone the repository
Project uses submodules for it's dependencies, so run:

``` shell
git clone https://github.com/omerfuyar/OpenECS.git
cd OpenECS
git submodule update --init dependencies/shu dependencies/shuild dependencies/SDL_net dependencies/SDL_image dependencies/SDL_mixer
```

Don't use `--recurse-submodules` here. SDL_image and SDL_mixer keep the third party libraries they can
optionally use in their own `external/` directories, around 800 MB of sources this build never touches.

### Dependencies
Project uses SDL for OS abstraction. SDL and SDL_ttf are installed with the package manager of your system,
SDL_image, SDL_mixer and SDL_net are plain C and small enough that the build script compiles them from the
submodules instead.

So the whole list is SDL 3.4 or newer, SDL_ttf, `git` and a C compiler, `gcc` or `clang`. No CMake, no Make.

| System | SDL | Command |
| --- | --- | --- |
| Arch, Manjaro, Omarchy | 3.4.16 | `sudo pacman -S --needed base-devel git sdl3 sdl3_ttf` |
| Fedora 43 and newer | 3.4.16 | `sudo dnf install gcc git SDL3-devel SDL3_ttf-devel` |
| openSUSE Tumbleweed | 3.4.16 | `sudo zypper install gcc git SDL3-devel SDL3_ttf-devel` |
| Alpine edge | 3.4.16 | `sudo apk add build-base git sdl3-dev sdl3_ttf-dev` |
| Debian unstable | 3.4.16 | `sudo apt install build-essential git libsdl3-dev libsdl3-ttf-dev` |
| Ubuntu 26.04 and newer | 3.4.2 | `sudo apt install build-essential git libsdl3-dev libsdl3-ttf-dev` |
| macOS | 3.4.16 | `xcode-select --install` and `brew install git sdl3 sdl3_ttf` |
| Windows, MSYS2 UCRT64 | 3.4.16 | `pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-sdl3_ttf` |

Debian 13 with 3.2.10 and Ubuntu 25.10 with 3.2.20 are too old, SDL_image and SDL_mixer call SDL functions
that arrived in 3.4. Check what you have with `pkg-config --modversion sdl3`.

The two compiled libraries use the decoders that come inside them, so they need nothing installed:

- Images: PNG, JPG, BMP, GIF, PNM, QOI, SVG, TGA, XCF, XPM, XV, ANI, LBM and PCX. AVIF, JXL, TIFF and WEBP
  are off, those are the formats that need an external library.
- Audio: WAV, AIFF, VOC, AU, FLAC, MP3, OGG Vorbis and MIDI. Opus, MOD, WavPack, GME and FluidSynth are off
  for the same reason.

To turn one of the missing ones on, install that library and add its `LOAD_<format>` or `DECODER_<name>`
definition in `shuild.c`.

### Building from source
Project uses [shuild](https://github.com/omerfuyar/shuild) as its build system, so the build script is a C
file. Compile it once, after that it rebuilds itself whenever it changes.

``` shell
gcc shuild.c -o shuild
./shuild RD
```

The argument is the build type, `D` for debug, `R` for release, `RD` for release with debug info and `SR`
for minimum size. A second `S` or `D` argument is reserved for static and dynamic linking, it is accepted
but does nothing yet. Every build type keeps its own artifacts, the executable ends up in
`build/<type>/bin/`.

The first build compiles SDL_image, SDL_mixer and SDL_net too, 60 files and around 15 seconds. After that
only what you changed is rebuilt.
