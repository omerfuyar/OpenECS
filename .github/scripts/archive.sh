#!/bin/sh
# Packs a folder of dist/ into dist/NAME.zip for Windows, which opens zip files itself, or into dist/NAME.tar.gz, and removes the folder.
# Prints the archive's path.
set -eu

name="$1"
platform="$2"

# Git Bash has no zip, but GitHub's Windows runners have 7-Zip
case "$platform" in
windows-*)
    archive="$name.zip"
    rm -f "dist/$archive"
    if command -v zip > /dev/null; then
        (cd dist && zip -qr "$archive" "$name")
    else
        (cd dist && 7z a -tzip -bso0 "$archive" "$name")
    fi
    ;;
*)
    archive="$name.tar.gz"
    tar -C dist -czf "dist/$archive" "$name"
    ;;
esac

rm -rf "dist/$name"
echo "dist/$archive"
