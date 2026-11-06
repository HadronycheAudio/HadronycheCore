#!/bin/bash
# Extracts the real download where a user would, reads what its binary needs, and runs the validator and all the tests
# on the extracted copy. Run it where there's an X server, such as through xvfb-run, because of panel-opens.
#
# Usage: test-tarball.sh <name> <tarball> <VST3 validator> <test program> <newest glibc allowed>
set -euo pipefail

name=$1
tarball=$2
validator=$3
tests=$4
newest_glibc=$5

assert () {
    local what=$1
    shift
    if ! "$@"; then
        echo "::error::Tarball test failed: $what"
        exit 1
    fi
    echo "ok   $what"
}

home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
mkdir -p "$home/.vst3"
tar -xzf "$tarball" -C "$home/.vst3"

bundle=$home/.vst3/$name.vst3
binary=$bundle/Contents/x86_64-linux/$name.so

assert 'the bundle is the only thing at the top of the tarball' test "$(ls "$home/.vst3")" = "$name.vst3"
assert 'the binary is where hosts look' test -f "$binary"
assert 'the module information is in place' test -f "$bundle/Contents/Resources/moduleinfo.json"
for licence in "$name-LICENSE.txt" VST3-SDK-LICENSE.txt VSTGUI-LICENSE.txt HadronycheCore-LICENSE.txt NOTICE.txt; do
    assert "$licence is in the bundle" test -f "$bundle/Contents/Resources/Licenses/$licence"
done

# What makes it run on an older distribution, which nothing else would notice on a new one.
needed=$(objdump -T "$binary" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)
echo "     it needs glibc $needed"
at_most_newest () { [ "$(printf '%s\n%s\n' "$needed" "$newest_glibc" | sort -V | tail -1)" = "$newest_glibc" ]; }
assert "that's no newer than glibc $newest_glibc" at_most_newest

no_cxx_runtime () { ! objdump -p "$binary" | grep -qE 'NEEDED +lib(stdc\+\+|gcc_s)'; }
assert "it carries its own C++ runtime" no_cxx_runtime

# Anything else exported could clash with the host or with other plug-ins.
exported=$(nm -D --defined-only "$binary" | awk '{ print $3 }' | sort | tr '\n' ' ')
assert 'it only exports the three entry points' test "$exported" = 'GetPluginFactory ModuleEntry ModuleExit '

missing=$(ldd "$binary" | grep 'not found' || true)
assert 'every library it needs is here' test -z "$missing"

"$validator" "$bundle"
"$tests" "$binary"
