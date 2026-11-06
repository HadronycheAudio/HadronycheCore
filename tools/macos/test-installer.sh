#!/bin/bash
# Runs the real installer on this machine, checks the installed plug-in with the validators and all the tests, then
# removes it and checks that it's gone. CI runs this before publishing a release. It writes to /Library, so it needs
# sudo and is meant for CI machines.
#
# Usage: test-installer.sh <name> <installer .pkg> <VST3 validator> <test program> <AU type> <AU subtype> <oldest macOS>
set -euo pipefail

name=$1
installer=$2
validator=$3
tests=$4
au_type=$5
au_subtype=$6
oldest=$7

lower=$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')
vst3=/Library/Audio/Plug-Ins/VST3/$name.vst3
component=/Library/Audio/Plug-Ins/Components/$name.component

assert () {
    local what=$1
    shift
    if ! "$@"; then
        echo "::error::Installer test failed: $what"
        exit 1
    fi
    echo "ok   $what"
}

# The copy that the build step leaves in the user's folder would otherwise stand in for the installed one.
rm -rf "$HOME/Library/Audio/Plug-Ins/Components/$name.component"

sudo installer -pkg "$installer" -target /

assert 'the VST3 is in the system folder' test -f "$vst3/Contents/MacOS/$name"
assert 'the Audio Unit is in the system folder' test -f "$component/Contents/MacOS/$name"
assert 'the Audio Unit holds a real VST3, not a link' test ! -L "$component/Contents/Resources/plugin.vst3"
assert 'with its binary inside' test -f "$component/Contents/Resources/plugin.vst3/Contents/MacOS/$name"

for bundle in "$vst3" "$component"; do
    for licence in "$name-LICENSE.txt" VST3-SDK-LICENSE.txt VSTGUI-LICENSE.txt HadronycheCore-LICENSE.txt AudioUnitSDK-LICENSE.txt NOTICE.txt; do
        assert "$licence is in $(basename "$bundle")" test -f "$bundle/Contents/Resources/Licenses/$licence"
    done
    assert "$(basename "$bundle")'s signature is intact" codesign --verify --deep --strict "$bundle"
done

# What makes it run on any Mac, which nothing else would notice on a new one. A build defaults to needing a macOS as
# new as the machine it's built on.
for binary in "$vst3/Contents/MacOS/$name" "$component/Contents/MacOS/$name"; do
    for arch in x86_64 arm64; do
        assert "$(basename "$(dirname "$(dirname "$(dirname "$binary")")")") has $arch" lipo "$binary" -verify_arch "$arch"
        minos=$(vtool -arch "$arch" -show-build "$binary" | awk '/minos/ { print $2 } /^ *version / { print $2 }' | head -1)
        echo "     it needs macOS $minos on $arch"
        at_most_oldest () { [ "$(printf '%s\n%s\n' "$minos" "$oldest" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" = "$oldest" ]; }
        if [ "$arch" = x86_64 ]; then
            assert "it runs on macOS $oldest" at_most_oldest
        fi
    done
    # Libraries are indented, and each architecture of a universal binary has a heading above them.
    unusual=$(otool -arch all -L "$binary" | awk '/^[[:space:]]/ { print $1 }' | grep -vE '^(/System/Library/|/usr/lib/)' || true)
    assert "it only needs what's part of macOS" test -z "$unusual"
done

"$validator" "$vst3"

# macOS caches its list of Audio Units, and this makes it look again.
killall -9 AudioComponentRegistrar 2> /dev/null || true
sleep 5
auval -a | grep -i hadronyche
auval -strict -v "$au_type" "$au_subtype" Hdrn

"$tests" "$vst3/Contents/MacOS/$name"

sudo rm -rf "$vst3" "$component"
sudo pkgutil --forget "com.hadronyche.$lower"
assert 'the VST3 is gone' test ! -e "$vst3"
assert 'the Audio Unit is gone' test ! -e "$component"
