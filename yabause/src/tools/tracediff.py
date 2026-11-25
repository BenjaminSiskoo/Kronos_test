#!/usr/bin/env python3
"""tracediff.py - compare two Kronos SH-2 / bus traces (see ctrl/include/trace/trace.h).

Usage:
  tracediff.py A.txt B.txt [options]

Both files are read as streams of events. Lines are normalised first (see
--ignore), then compared in order. At the first divergence the script prints
the preceding common lines and the next lines of each trace; with --max-diffs N
it tries to re-synchronise after a divergence (searching a small window for a
run of equal lines) and reports up to N hunks.

Options:
  --ignore LIST    comma list among: cyc (cycle counters), regs (register dump
                   on instruction lines), bus (all bus lines), val (bus values,
                   keep only direction/size/address), frame (# frame markers)
  --cpu M|S        compare only this CPU
  --context N      common lines shown before a hunk (default 6)
  --show N         lines shown after the divergence in each trace (default 8)
  --max-diffs N    number of hunks to report (default 1)
  --window N       re-synchronisation window in lines (default 400)
  --pc-only        compare only the sequence of executed PCs (same as
                   --ignore cyc,regs,bus,frame)
Exit status: 0 identical, 1 different, 2 usage / read error.
"""
import argparse
import re
import sys

REG = re.compile(r' (?:r\d+|sr|pr|gbr|vbr|mach|macl)=[0-9A-Fa-f]+')
CYC = re.compile(r' cyc=\d+')


def load(path, ign, cpu):
    out = []
    try:
        f = open(path, 'r', errors='replace')
    except OSError as e:
        sys.exit("cannot read %s: %s" % (path, e))
    with f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line:
                continue
            if line.startswith('#'):
                if line.startswith('# frame') and 'frame' not in ign:
                    out.append((n, line, line))
                continue
            parts = line.split(' ')
            if len(parts) < 3:
                continue
            if cpu and parts[0] != cpu:
                continue
            kind = parts[1]
            norm = line
            if kind == 'I':
                if 'cyc' in ign:
                    norm = CYC.sub('', norm)
                if 'regs' in ign:
                    norm = REG.sub('', norm)
            else:
                if 'bus' in ign:
                    continue
                if 'val' in ign and len(parts) >= 4:
                    norm = ' '.join(parts[:3])
            out.append((n, line, norm))
    return out


def resync(a, b, i, j, win, run=4):
    """Smallest (x, y) so that a[i+x:i+x+run] == b[j+y:j+y+run]."""
    best = None
    for s in range(0, 2 * win):
        for x in range(0, min(s, win) + 1):
            y = s - x
            if y > win:
                continue
            ia, jb = i + x, j + y
            if ia + run > len(a) or jb + run > len(b):
                continue
            if all(a[ia + k][2] == b[jb + k][2] for k in range(run)):
                return x, y
    return best


def show(tag, ev, start, count):
    for k in range(start, min(start + count, len(ev))):
        n, raw, _ = ev[k]
        print("  %s:%-8d %s" % (tag, n, raw))
    if start >= len(ev):
        print("  %s: <end of trace>" % tag)


def main():
    ap = argparse.ArgumentParser(description="Compare two Kronos SH-2/bus traces")
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--ignore', default='')
    ap.add_argument('--cpu', choices=['M', 'S', 'm', 's'])
    ap.add_argument('--context', type=int, default=6)
    ap.add_argument('--show', type=int, default=8)
    ap.add_argument('--max-diffs', type=int, default=1)
    ap.add_argument('--window', type=int, default=400)
    ap.add_argument('--pc-only', action='store_true')
    o = ap.parse_args()
    ign = set(x for x in o.ignore.split(',') if x)
    if o.pc_only:
        ign |= {'cyc', 'regs', 'bus', 'frame'}
    cpu = o.cpu.upper() if o.cpu else None
    a = load(o.a, ign, cpu)
    b = load(o.b, ign, cpu)
    print("A: %s (%d events)\nB: %s (%d events)" % (o.a, len(a), o.b, len(b)))

    i = j = 0
    hunks = 0
    same = 0
    while i < len(a) and j < len(b):
        if a[i][2] == b[j][2]:
            i += 1; j += 1; same += 1
            continue
        hunks += 1
        print("\n=== difference %d after %d identical events ===" % (hunks, same))
        c0 = max(0, i - o.context)
        for k in range(c0, i):
            print("   =  %-8d %s" % (a[k][0], a[k][1]))
        print("A:"); show("A", a, i, o.show)
        print("B:"); show("B", b, j, o.show)
        if hunks >= o.max_diffs:
            break
        r = resync(a, b, i + 1, j + 1, o.window)
        if r is None:
            print("\n(no re-synchronisation within %d lines)" % o.window)
            break
        x, y = r
        print("\n  -> re-synchronised: A skips %d event(s), B skips %d event(s)" % (x + 1, y + 1))
        i += x + 1; j += y + 1
        same = 0
    else:
        if i < len(a) or j < len(b):
            hunks += 1
            print("\n=== one trace ends first (A: %d left, B: %d left) ===" % (len(a) - i, len(b) - j))
            if i < len(a):
                print("A next:"); show("A", a, i, o.show)
            else:
                print("B next:"); show("B", b, j, o.show)

    if hunks == 0:
        print("traces identical (%d events)" % len(a))
        return 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
