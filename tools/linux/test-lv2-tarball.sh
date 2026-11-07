#!/bin/bash
# Extracts a plug-in's real LV2 download where a user would, reads what its binary needs, and compares its output with the
# VST3 from the other download, bit for bit.
#
# Usage: test-lv2-tarball.sh <name> <LV2 tarball> <VST3 tarball> <Lv2Compare program> <newest glibc allowed>
set -euo pipefail

name=$1
tarball=$2
vst3_tarball=$3
compare=$4
newest_glibc=$5

assert () {
    local what=$1
    shift
    if ! "$@"; then
        echo "::error::LV2 tarball test failed: $what"
        exit 1
    fi
    echo "ok   $what"
}

home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
mkdir -p "$home/.lv2" "$home/.vst3"
tar -xzf "$tarball" -C "$home/.lv2"
tar -xzf "$vst3_tarball" -C "$home/.vst3"

bundle=$home/.lv2/$name.lv2
binary=$bundle/$name.so

assert 'the bundle is the only thing at the top of the tarball' test "$(ls "$home/.lv2")" = "$name.lv2"
assert 'the binary is where the manifest says' test -f "$binary"
assert 'the manifest is in place' test -f "$bundle/manifest.ttl"
assert 'the description is in place' test -f "$bundle/$name.ttl"
for licence in "$name-LICENSE.txt" VST3-SDK-LICENSE.txt VSTGUI-LICENSE.txt HadronycheCore-LICENSE.txt LV2-LICENSE.txt NOTICE.txt; do
    assert "$licence is in the bundle" test -f "$bundle/Licenses/$licence"
done

# What makes it run on an older distribution, which nothing else would notice on a new one.
needed=$(objdump -T "$binary" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)
echo "     it needs glibc $needed"
at_most_newest () { [ "$(printf '%s\n%s\n' "$needed" "$newest_glibc" | sort -V | tail -1)" = "$newest_glibc" ]; }
assert "that's no newer than glibc $newest_glibc" at_most_newest

no_cxx_runtime () { ! objdump -p "$binary" | grep -qE 'NEEDED +lib(stdc\+\+|gcc_s)'; }
assert 'it carries its own C++ runtime' no_cxx_runtime

# Anything else exported could clash with the host or with other plug-ins.
exported=$(nm -D --defined-only "$binary" | awk '{ print $3 }' | sort | tr '\n' ' ')
assert 'it only exports the two descriptors' test "$exported" = 'lv2_descriptor lv2ui_descriptor '

missing=$(ldd "$binary" | grep 'not found' || true)
assert 'every library it needs is here' test -z "$missing"

"$compare" "$home/.vst3/$name.vst3/Contents/x86_64-linux/$name.so" "$bundle"
