#!/bin/bash
# Dumps one built plug-in into a folder: everything NullTest sees, plus the panel's screenshots.
#
# Usage: dump-plugin.sh <NullTest program> <tests program> <the plug-in's .vst3 bundle> <NAME for the screenshot variable> <output folder>
#
# The folder is replaced. The tests program has to be one built for this plug-in, and it's only used for its panel
# test, which only exists on Windows.
set -u

nulltest=$1
tests=$2
bundle=$3
name=$4
output=$5

base=$(basename "$bundle")
if [ "$(uname -s)" = Linux ]; then
    dll="$bundle/Contents/$(uname -m)-linux/${base%.vst3}.so"
    native () { printf '%s' "$1"; }
else
    dll="$bundle/Contents/x86_64-win/$base"
    native () { cygpath -w "$1"; }
fi

rm -rf "$output"
mkdir -p "$output/panel"

"$nulltest" "$(native "$dll")" "$(native "$output")" > "$output.log" 2>&1
status=$?
if [ $status -ne 0 ]; then
    echo "NullTest failed with $status, see $output.log"
    exit $status
fi

if [ "$(uname -s)" != Linux ]; then
    env "${name}_SCREENSHOTS=$(native "$output/panel")" "$tests" "$(native "$dll")" panel >> "$output.log" 2>&1
    status=$?
    if [ $status -ne 0 ]; then
        echo "The panel test failed with $status, see $output.log"
        exit $status
    fi
fi

echo "$base: $(find "$output" -type f | wc -l) files"
