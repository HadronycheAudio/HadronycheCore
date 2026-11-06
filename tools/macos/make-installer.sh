#!/bin/bash
# Builds a plug-in's macOS installer: a .pkg that puts its VST3 and Audio Unit in the system folders where hosts look.
#
# Usage, from the plug-in's folder, after building it in Release in build/:
#   make-installer.sh <name> <version> <output folder>
#
# Nothing is signed with an Apple account, so macOS warns about the installer the first time it's opened.
set -euo pipefail

name=$1
version=$2
output=$3

here=$(cd "$(dirname "$0")" && pwd)
lower=$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')
built=build/VST3/Release
audio_unit_sdk=$(sed -n 's/^SMTG_AUDIOUNIT_SDK_PATH:PATH=//p' build/CMakeCache.txt)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

payload=$work/payload
vst3=$payload/Library/Audio/Plug-Ins/VST3/$name.vst3
component=$payload/Library/Audio/Plug-Ins/Components/$name.component
mkdir -p "$(dirname "$vst3")" "$(dirname "$component")" "$output"

# ditto keeps the bundles exactly as they are. The Audio Unit holds the VST3 as a link into the build folder, which
# wouldn't survive being installed, so it gets a real copy.
ditto "$built/$name.vst3" "$vst3"
ditto "$built/$name.component" "$component"
rm -rf "$component/Contents/Resources/plugin.vst3"
ditto "$built/$name.vst3" "$component/Contents/Resources/plugin.vst3"

# The licences have to accompany the binaries.
for bundle in "$vst3" "$component"; do
    licences=$bundle/Contents/Resources/Licenses
    mkdir -p "$licences"
    cp LICENSE "$licences/$name-LICENSE.txt"
    cp external/vst3sdk/LICENSE.txt "$licences/VST3-SDK-LICENSE.txt"
    cp external/vst3sdk/vstgui4/LICENSE "$licences/VSTGUI-LICENSE.txt"
    cp external/hadronychecore/LICENSE "$licences/HadronycheCore-LICENSE.txt"
    cp "$audio_unit_sdk/LICENSE.txt" "$licences/AudioUnitSDK-LICENSE.txt"
    cp installer/NOTICE.txt "$licences/NOTICE.txt"
    if [ -d licenses ]; then
        for extra in licenses/*; do
            cp "$extra" "$licences/$(basename "$extra")"
        done
    fi
done

# After everything's in place, because signing seals a bundle's contents. Inside out, for the same reason.
codesign --force -s - "$component/Contents/Resources/plugin.vst3"
codesign --force -s - "$component"
codesign --force -s - "$vst3"

# Bundles must go where they're meant to, and not to a copy that happens to exist somewhere else on the disk.
pkgbuild --analyze --root "$payload" "$work/components.plist"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$work/components.plist" > /dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$work/components.plist" 2> /dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$work/components.plist"
    i=$((i + 1))
done

# Without this, the Audio Unit wouldn't show up until the next login.
mkdir -p "$work/scripts"
cat > "$work/scripts/postinstall" <<'EOF'
#!/bin/bash
killall -9 AudioComponentRegistrar 2> /dev/null
exit 0
EOF
chmod +x "$work/scripts/postinstall"

pkgbuild --root "$payload" \
    --component-plist "$work/components.plist" \
    --identifier "com.hadronyche.$lower" \
    --version "$version" \
    --install-location / \
    --scripts "$work/scripts" \
    "$work/$name.pkg"

mkdir -p "$work/resources"
cp LICENSE "$work/resources/LICENSE.txt"
sed -e "s/@NAME@/$name/g" -e "s/@LOWER@/$lower/g" -e "s/@VERSION@/$version/g" "$here/distribution.xml.in" > "$work/distribution.xml"

productbuild --distribution "$work/distribution.xml" \
    --resources "$work/resources" \
    --package-path "$work" \
    "$output/$name-$version-macOS.pkg"

echo "Built $output/$name-$version-macOS.pkg"
