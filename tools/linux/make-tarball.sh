#!/bin/bash
# Builds a plug-in's Linux download: a tarball holding its .vst3 bundle, to extract into ~/.vst3 or /usr/lib/vst3.
#
# Usage, from the plug-in's folder, after building it in Release in build/:
#   make-tarball.sh <name> <version> <output folder>
set -euo pipefail

name=$1
version=$2
output=$3

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp -r "build/VST3/Release/$name.vst3" "$work/"

# The licences have to accompany the binaries.
licences=$work/$name.vst3/Contents/Resources/Licenses
mkdir -p "$licences" "$output"
cp LICENSE "$licences/$name-LICENSE.txt"
cp external/vst3sdk/LICENSE.txt "$licences/VST3-SDK-LICENSE.txt"
cp external/vst3sdk/vstgui4/LICENSE "$licences/VSTGUI-LICENSE.txt"
cp external/hadronychecore/LICENSE "$licences/HadronycheCore-LICENSE.txt"
cp installer/NOTICE.txt "$licences/NOTICE.txt"
if [ -d licenses ]; then
    for extra in licenses/*; do
        cp "$extra" "$licences/$(basename "$extra")"
    done
fi

file=$name-$version-Linux-x86_64.tar.gz
tar -czf "$output/$file" --owner=0 --group=0 -C "$work" "$name.vst3"

echo "Built $output/$file"
