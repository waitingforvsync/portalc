#!/usr/bin/env python3
"""uncovered.py - the lines llvm-cov's `show` says no golden executed,
grouped by file and function (PLAN.md 5.5).

    uncovered.py SHOW.txt [REVIEWED.txt]

A line counts as uncovered when llvm-cov gives it an execution count of 0.
Lines with no count (declarations, blank lines, comments) are skipped.
With REVIEWED (host/uncovered.txt), lines it lists are marked, and the exit
status is 1 when any uncovered line isn't listed, or a listed one now runs.
"""

import re
import sys

LINE = re.compile(r'^\s*(\d+)\|\s*([0-9.kMG]*)\|(.*)$')
FUNC = re.compile(r'^(?:static\s+)?[a-z_][\w\s\*]*?\b([a-z_]\w*)\s*\([^;]*$')


def reviewed(path):
    """FILE:LINE or FILE:FIRST-LAST entries, as a set of (file, line)."""
    out = set()
    for raw in open(path, encoding='utf-8'):
        raw = raw.split('#', 1)[0].strip()
        if not raw:
            continue
        where = raw.split()[0]
        file, lines = where.split(':')
        first, _, last = lines.partition('-')
        for n in range(int(first), int(last or first) + 1):
            out.add((file, n))
    return out


def main(path, known_path=None):
    current_file = None
    current_func = None
    out = {}
    ran = set()
    for raw in open(path, encoding='utf-8', errors='replace'):
        raw = raw.rstrip('\n')
        if raw.endswith('.c:') and raw.startswith('/'):
            current_file = raw[:-1].rsplit('/', 1)[-1]
            current_func = None
            continue
        m = LINE.match(raw)
        if not m or current_file is None:
            continue
        num, count, src = int(m.group(1)), m.group(2), m.group(3)
        f = FUNC.match(src)
        if f and not src.startswith(' '):
            current_func = f.group(1)
        if count == '0':
            out.setdefault((current_file, current_func), []).append((num, src.strip()))
        elif count:
            ran.add((current_file, num))
    known = reviewed(known_path) if known_path else set()
    fresh = 0
    for (file, func), lines in out.items():
        print('%s: %s' % (file, func))
        for num, src in lines:
            listed = (file, num) in known
            fresh += not listed
            print('%s %5d  %s' % (' ' if listed or not known_path else '!', num, src))
    if not known_path:
        return 0
    gone = sorted(known & ran)
    for file, num in gone:
        print('now covered, drop from the list: %s:%d' % (file, num))
    if fresh:
        print('%d uncovered lines not in the reviewed list (marked !)' % fresh)
    return 1 if fresh or gone else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]))
