#!/bin/sh
# Copies the licenses of the works that the standard plugins include or link, and the font draw ships, into a folder,
# and prints a line for each, for the folder's README.txt. OpenECS's release and OpenECS-std's ship them.
set -eu

folder="$1"
std=std/dependencies

cp $std/SDL_net/LICENSE.txt "$folder/SDL_net.txt"
cp $std/nanosvg/LICENSE.txt "$folder/nanosvg.txt"
cp $std/dr_libs/LICENSE "$folder/dr_libs.txt"
cp $std/cgltf/LICENSE "$folder/cgltf.txt"
cp $std/cglm/LICENSE "$folder/cglm.txt"
cp $std/curl/COPYING "$folder/curl.txt"
cp $std/mbedtls/LICENSE "$folder/mbedTLS.txt"
cp std/plugins/draw/fonts/OFL.txt "$folder/RobotoMono.txt"

cat << NOTICE
SDL_net: zlib license
nanosvg: zlib license
dr_libs (dr_wav, dr_mp3, dr_flac): MIT No Attribution license or public domain
cgltf: MIT license
cglm: MIT license
curl (libcurl): curl license, derived from the MIT license
Mbed TLS: Apache License 2.0, chosen of its Apache 2.0 or GPL 2.0 licenses
Roboto Mono font: SIL Open Font License 1.1
NOTICE
