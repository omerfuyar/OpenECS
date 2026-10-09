#!/bin/sh
# Prints the version of OpenECS from OPENECS_VERSION in include/OpenECS.h.
# For a version tag, such as v0.1.0, it fails unless the tag names the same version, so a release never carries another number.
set -eu

version=$(sed -n 's/^#define OPENECS_VERSION "\(.*\)"$/\1/p' include/OpenECS.h)

if [ "${GITHUB_REF_TYPE:-}" = "tag" ] && [ "${GITHUB_REF_NAME:-}" != "v$version" ]; then
    echo "The tag $GITHUB_REF_NAME does not match OPENECS_VERSION $version in include/OpenECS.h." >&2
    exit 1
fi

echo "$version"
