#!/usr/bin/env python3
"""Compares two folders dumped by dump-plugin.sh, and exits with 0 only if they match.

Usage: compare.py <baseline folder> <folder to check> [--limit N] [--audio-tolerance DBFS]
                  [--panel-ignore x0,y0,x1,y1]... [--panel-tolerance PIXELS]

Audio is compared bit for bit. Where it differs, this reports how much by, relative to full scale,
because the SDK builds with /fp:fast, which allows tiny differences that are still worth knowing about.
With --audio-tolerance, a difference whose peak is at or below that level, such as -120, counts as a
match and is only noted. That's for a change that can't be bit for bit, such as the same arithmetic
through another language's maths library, and the note says how far off it is.

Panel screenshots are exact by default. The panel tests run in real time, so a panel showing something
that moves needs its region ignored, and its few redrawn pixels allowed for with a tolerance. Anything
ignored here has to be looked at by eye.
"""

import array
import math
import os
import struct
import sys


def walk(folder):
    found = set()
    for root, _, names in os.walk(folder):
        for name in names:
            found.add(os.path.relpath(os.path.join(root, name), folder).replace("\\", "/"))
    return found


def read(path):
    with open(path, "rb") as file:
        return file.read()


def describe_audio(a, b):
    """Returns what differs, and the peak difference in dBFS, or None where nothing is comparable."""
    left = array.array("f")
    right = array.array("f")
    left.frombytes(a[: len(a) // 4 * 4])
    right.frombytes(b[: len(b) // 4 * 4])

    if len(left) != len(right):
        return f"different lengths: {len(left)} and {len(right)} values", None

    differing = 0
    first = -1
    worst = 0.0
    only_nan_in_one = 0
    for i, (x, y) in enumerate(zip(left, right)):
        if x == y or (math.isnan(x) and math.isnan(y)):
            continue
        differing += 1
        if first < 0:
            first = i
        difference = abs(x - y)
        if math.isnan(difference) or math.isinf(difference):
            only_nan_in_one += 1
        else:
            worst = max(worst, difference)

    if differing == 0:
        return "the same values, with different NaN or zero bit patterns", -math.inf

    peak = -math.inf if worst == 0.0 else 20.0 * math.log10(worst)
    level = "silence" if worst == 0.0 else f"{peak:.1f} dBFS"
    text = f"{differing} of {len(left)} values differ, first at frame {first // 2}, peak difference {level}"
    if only_nan_in_one:
        text += f", {only_nan_in_one} where only one side is NaN or infinite"
        peak = None
    return text, peak


def compare_bitmaps(a, b, ignore):
    """Returns the differing pixels outside the ignored regions, and the area they cover."""
    if len(a) != len(b):
        return -1, None

    offset = struct.unpack("<I", a[10:14])[0]
    width, height = struct.unpack("<ii", a[18:26])
    height = abs(height)

    count = 0
    xs = []
    ys = []
    for y in range(height):
        start = offset + y * width * 4
        if a[start : start + width * 4] == b[start : start + width * 4]:
            continue
        for x in range(width):
            at = start + x * 4
            if a[at : at + 3] == b[at : at + 3]:
                continue
            if any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in ignore):
                continue
            count += 1
            xs.append(x)
            ys.append(y)

    area = (min(xs), min(ys), max(xs), max(ys)) if xs else None
    return count, area


def main():
    arguments = sys.argv[1:]
    limit = 40
    tolerance = 0
    audio_tolerance = None
    ignore = []

    def take(flag):
        index = arguments.index(flag)
        value = arguments[index + 1]
        del arguments[index : index + 2]
        return value

    while "--limit" in arguments:
        limit = int(take("--limit"))
    while "--panel-tolerance" in arguments:
        tolerance = int(take("--panel-tolerance"))
    while "--audio-tolerance" in arguments:
        audio_tolerance = float(take("--audio-tolerance"))
    while "--panel-ignore" in arguments:
        ignore.append(tuple(int(part) for part in take("--panel-ignore").split(",")))

    if len(arguments) != 2:
        print(__doc__)
        return 2

    baseline, candidate = arguments
    left = walk(baseline)
    right = walk(candidate)

    if not left:
        print(f"Nothing to compare in {baseline}")
        return 2

    problems = []
    notes = []
    for name in sorted(left - right):
        problems.append(f"MISSING  {name}")
    for name in sorted(right - left):
        problems.append(f"EXTRA    {name}")

    same = 0
    for name in sorted(left & right):
        a = read(os.path.join(baseline, name))
        b = read(os.path.join(candidate, name))
        if a == b:
            same += 1
        elif name.endswith(".f32"):
            text, peak = describe_audio(a, b)
            if audio_tolerance is not None and peak is not None and peak <= audio_tolerance:
                same += 1
                notes.append(f"note     {name}: {text}")
            else:
                problems.append(f"AUDIO    {name}: {text}")
        elif name.endswith(".bmp"):
            count, area = compare_bitmaps(a, b, ignore)
            if count < 0:
                problems.append(f"PANEL    {name}: different sizes")
            elif count > tolerance:
                problems.append(f"PANEL    {name}: {count} pixels differ, within {area}")
            else:
                same += 1
                notes.append(f"note     {name}: {count} pixels differ outside the ignored regions")
        else:
            problems.append(f"DIFFERS  {name}: {len(a)} and {len(b)} bytes")

    for note in notes:
        print(note)
    for problem in problems[:limit]:
        print(problem)
    if len(problems) > limit:
        print(f"... and {len(problems) - limit} more")

    print(f"{same} match, {len(problems)} different")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
