#!/bin/sh
# Packs a Release build for one platform into dist/openecs-VERSION-PLATFORM.tar.gz, or .zip for Windows:
# the program with its plugins, presets, examples and resources; the plugin interface with the standard plugins' definition files; OpenECS's license and the licenses of what it includes.
# For Windows, also the import libraries that a C plugin links (DESIGN 17.2).
set -eu

version="$1"
platform="$2"
name="openecs-$version-$platform"
folder="dist/$name"

rm -rf "$folder"
mkdir -p "$folder/include" "$folder/licenses"

cp -r build/Release/bin/. "$folder/"
cp include/OpenECS.h include/ecs.lua "$folder/include/"
cp -r build/Release/include/shu "$folder/include/"
# cglm's headers, which the cglm plugin's definition header includes, so C plugins call cglm
cp -r build/Release/include/cglm "$folder/include/"
# the build wrote the definition files of the plugins it found, the standard ones, so editors complete their functions with include/ alone
cp build/Release/definitions/*.lua build/Release/definitions/*.h "$folder/include/"
cp LICENSE.md README.md "$folder/"

# the plugins' own import libraries are left out, as nothing links a plugin
find "$folder" -name '*.dll.a' -delete

case "$platform" in
windows-*)
    mkdir -p "$folder/lib"
    cp build/Release/lib/libOpenECS.a build/Release/lib/libSDL3.dll.a build/Release/lib/libSDL3_ttf.dll.a "$folder/lib/"
    ;;
esac

# the libraries linked into the program, and the font it ships
ttf=dependencies/SDL_ttf/external
cp dependencies/SDL/LICENSE.txt "$folder/licenses/SDL.txt"
cp dependencies/SDL_ttf/LICENSE.txt "$folder/licenses/SDL_ttf.txt"
cp "$ttf/freetype/docs/FTL.TXT" "$folder/licenses/FreeType.txt"
cp "$ttf/harfbuzz/COPYING" "$folder/licenses/HarfBuzz.txt"
cp "$ttf/plutosvg/LICENSE" "$folder/licenses/PlutoSVG.txt"
cp "$ttf/plutovg/LICENSE" "$folder/licenses/PlutoVG.txt"
sed -n '/^\* Copyright (C) 1994/,/^\*\*\*/p' dependencies/lua/lua.h | sed 's/^\* \{0,1\}//' > "$folder/licenses/Lua.txt"
cp dependencies/libffi/LICENSE "$folder/licenses/libffi.txt"
cp dependencies/clay/LICENSE.md "$folder/licenses/Clay.txt"
cp dependencies/stb/LICENSE "$folder/licenses/stb.txt"
cp resources/Roboto-LICENSE.txt "$folder/licenses/Roboto.txt"

# the libraries that the standard plugins include or link, and the font draw ships
std_notices=$(.github/scripts/licenses-std.sh "$folder/licenses")

cat > "$folder/licenses/README.txt" << NOTICE
OpenECS $version includes these works. Their licenses are in this folder.

SDL, SDL_ttf: zlib license
FreeType: FreeType License. Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.
HarfBuzz: MIT license
PlutoSVG, PlutoVG: MIT license
Lua: MIT license
libffi: MIT license
Clay: zlib license
stb (stb_image, stb_vorbis): MIT license or public domain
Roboto font: Apache License 2.0
$std_notices
NOTICE

.github/scripts/archive.sh "$name" "$platform"
