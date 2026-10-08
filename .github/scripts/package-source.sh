#!/bin/sh
# Packs the source with every submodule into dist/openecs-VERSION-source.tar.gz.
# GitHub's own source archives leave the submodules out, so they cannot be built.
set -eu

version="$1"
name="openecs-$version-source"

mkdir -p dist
git ls-files --recurse-submodules -z | tar --null -T - --transform "s,^,$name/," -czf "dist/$name.tar.gz"
echo "dist/$name.tar.gz"
