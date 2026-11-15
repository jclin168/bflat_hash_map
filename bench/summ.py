#!/usr/bin/env python3
"""Median (and min..max) over rounds of each metric per (kind, map, n)."""
import re, sys, statistics, collections
path = sys.argv[1]
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(path):
    m = re.match(r'round=(\d+) (\w+) map=(\S+) n=(\d+) (.*)', line.strip())
    if not m: continue
    _, kind, mp, n, rest = m.groups()
    for k, v in re.findall(r'(\w+)=([\d.]+)', rest):
        rows[(kind, int(n), mp)][k].append(float(v))
for key in sorted(rows):
    kind, n, mp = key
    parts = []
    for k, vs in rows[key].items():
        med = statistics.median(vs)
        if len(vs) > 1 and min(vs) != max(vs):
            parts.append(f"{k}={med:g} [{min(vs):g}..{max(vs):g}]")
        else:
            parts.append(f"{k}={med:g}")
    print(f"{kind} n={n} map={mp} rounds={len(next(iter(rows[key].values())))} " + " ".join(parts))
