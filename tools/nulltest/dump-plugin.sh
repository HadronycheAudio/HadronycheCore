#!/bin/bash
# Dumps one built plug-in into a folder: everything NullTest sees, plus the panel's screenshots.
#
# Usage: dump-plugin.sh <NullTest.exe> <tests exe> <the plug-in's .vst3 bundle> <NAME for the screenshot variable> <output folder>
#
# The folder is replaced. The tests exe has to be one built for this plug-in, and it's only used for its panel test.
set -u

nulltest=$1
tests=$2
bundle=$3
name=$4
output=$5

base=$(basename "$bundle")
dll="$bundle/Contents/x86_64-win/$base"

rm -rf "$output"
mkdir -p "$output/panel"

"$nulltest" "$(cygpath -w "$dll")" "$(cygpath -w "$output")" > "$output.log" 2>&1
status=$?
if [ $status -ne 0 ]; then
    echo "NullTest failed with $status, see $output.log"
    exit $status
fi

env "${name}_SCREENSHOTS=$(cygpath -w "$output/panel")" "$tests" "$(cygpath -w "$dll")" panel >> "$output.log" 2>&1
status=$?
if [ $status -ne 0 ]; then
    echo "The panel test failed with $status, see $output.log"
    exit $status
fi

echo "$base: $(find "$output" -type f | wc -l) files"
