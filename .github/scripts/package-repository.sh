#!/bin/sh
# Packs the part of a Release build that OpenECS-std or OpenECS-examples makes, for that repository's own release:
# dist/openecs-REPOSITORY-VERSION-PLATFORM.tar.gz, or .zip for Windows. Unpacked into an OpenECS folder of the same version, its folders join OpenECS's.
# std: the plugins, the preset launcher, and include/ with the plugins' definition files, beside the plugin interface of the same version.
# examples: the examples, with their native plugins built.
set -eu

repository="$1"
version="$2"
platform="$3"
name="openecs-$repository-$version-$platform"
folder="dist/$name"

rm -rf "$folder"
mkdir -p "$folder"

case "$repository" in
std)
    mkdir -p "$folder/licenses"
    cp -r build/Release/bin/plugins build/Release/bin/presets build/Release/include "$folder/"
    notices=$(.github/scripts/licenses-std.sh "$folder/licenses")
    printf 'OpenECS-std %s includes these works. Their licenses are in this folder.\n\n%s\n' "$version" "$notices" > "$folder/licenses/README.txt"
    ;;
examples)
    cp -r build/Release/bin/examples "$folder/"
    ;;
*)
    echo "Usage: package-repository.sh std|examples VERSION PLATFORM"
    exit 2
    ;;
esac

cp "$repository/LICENSE.md" "$repository/README.md" "$folder/"

# the plugins' own import libraries are left out, as nothing links a plugin
find "$folder" -name '*.dll.a' -delete

.github/scripts/archive.sh "$name" "$platform"
