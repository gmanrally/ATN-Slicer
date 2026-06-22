#!/usr/bin/env python3
"""ATTACK 1b: localized per-layer-pair interlock. Whole-loop RMS over 120 layers HIDES a
single scrambled weld plane. A real print fails at the WORST plane, not the average. So we
compute, per part, per island track, the interlock RMS at EVERY adjacent layer-pair and
report the WORST pair (and where it is). We also crank the chatter frequency to force many
merge/split + N-change events."""
import math
import numpy as np

import validate_weave2 as J
import importlib.util, sys, os
import scratch_attack1 as A

name = "island_prop"
spec = J._METHODS[name]


def per_pair_worst(layers, label):
    state = spec["prepare"](layers)
    rows = []
    for li in range(len(layers) - 1):
        Aisl = J._island_loops(layers[li])
        Bisl = J._island_loops(layers[li + 1])
        for iid, la in Aisl.items():
            if iid not in Bisl:
                continue
            lb = Bisl[iid]
            za = spec["field"](la, li, "nested", state)
            zb = spec["field"](lb, li + 1, "nested", state)
            j, dist = J._match_pts(la["xy"], lb["xy"], 0.5)
            ok = dist <= 0.5
            if not ok.any():
                continue
            e = za[ok] + zb[j[ok]]
            rms = 100.0 * math.sqrt(np.mean(e ** 2)) / J.AMP
            mx = 100.0 * np.max(np.abs(e)) / J.AMP
            rows.append((rms, mx, li, iid, int(ok.sum())))
    rows.sort(reverse=True)
    print(f"\n-- {label}: worst adjacent layer-pairs (interlock RMS%, maxerr%, layer, island, npts) --")
    for r in rows[:8]:
        print(f"   RMS={r[0]:6.1f}  max={r[1]:6.1f}  layer {r[2]:3d}->{r[2]+1:3d}  island {r[3]}  n={r[4]}")
    if rows:
        print(f"   WORST RMS over all pairs = {rows[0][0]:.1f}%   (whole-part avg interlock dilutes this)")
    return rows


def part_chatter_fast(nlayers=140, freq=5.0):
    """More aggressive: many merge/split cycles -> many births/deaths + N changes."""
    rng = np.random.default_rng(J.SEED + 25)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        phase = math.sin(freq * math.pi * z)
        sep = 18 + 14 * phase
        if sep > 9:
            loops.append((1, A._circle(-sep / 2, 0, 6.5)))
            loops.append((2, A._circle(+sep / 2, 0, 6.5)))
        else:
            loops.append((1, A._peanut(0, 0, max(2.0, sep), 6.5, 4.0, n=300)))
        if k % 2:
            loops = loops[::-1]
        layers.append([J._loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


def main():
    for pname, fac in (("split", A.part_split), ("cusp", A.part_cusp),
                       ("tangent", A.part_tangent), ("concentric", A.part_concentric),
                       ("chatter", A.part_chatter)):
        per_pair_worst(fac(), pname)
    print("\n==== cranked chatter ====")
    per_pair_worst(part_chatter_fast(), "chatter_fast")


if __name__ == "__main__":
    main()
