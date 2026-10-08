#!/bin/sh
# Packs a Release build for one platform into dist/openecs-VERSION-PLATFORM.tar.gz:
# the program with its plugins, presets and resources; the plugin interface; OpenECS's license and the licenses of what it includes.
set -eu

version="$1"
platform="$2"
name="openecs-$version-$platform"
folder="dist/$name"

rm -rf "$folder"
mkdir -p "$folder/include" "$folder/licenses"

cp -r build/Static/Release/bin/. "$folder/"
cp include/OpenECS.h include/ecs.lua "$folder/include/"
cp -r build/Static/Release/include/shu "$folder/include/"
cp LICENSE README.md "$folder/"

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

cat > "$folder/licenses/README.txt" << NOTICE
OpenECS $version includes these works. Their licenses are in this folder.

SDL, SDL_ttf: zlib license
FreeType: FreeType License. Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.
HarfBuzz: MIT license
PlutoSVG, PlutoVG: MIT license
Lua: MIT license
libffi: MIT license
Clay: zlib license
stb: MIT license or public domain
Roboto font: Apache License 2.0
NOTICE

tar -C dist -czf "dist/$name.tar.gz" "$name"
rm -rf "$folder"
echo "dist/$name.tar.gz"
