#!/usr/bin/env python3
"""ATTACK 1c: diagnose the chatter spike. Is the 81.8% worst-pair a GENUINE algorithm
failure (a real weld plane defect) or a test artifact (instantaneous/non-physical merge)?

We:
  1. Make the split PHYSICALLY SMOOTH (gradual neck shrink, never an instantaneous jump).
  2. Print, per layer pair, the candidate's per-track N, s0, reseed/birth flags and the
     interlock RMS, around the worst layers, so we can attribute the spike to a CAUSE:
       - birth (new track, fresh centroid seam)  -> req(1) adjacent registration break
       - N change (frequency mismatch)           -> unfixable single-layer ~100%
       - track swap / misbind                     -> cross-feature contamination
  3. Sanity: compare to a SMOOTH single-island circle (no events) to prove the harness
     itself reports ~0 there (so the spike is the candidate, not the metric)."""
import math
import numpy as np

import validate_weave2 as J
import scratch_attack1 as A

name = "island_prop"
spec = J._METHODS[name]


def _circle(cx, cy, r, n=None):
    return A._circle(cx, cy, r, n)


def part_split_smooth(nlayers=130):
    """Two roots with a GRADUAL neck: a peanut whose waist shrinks smoothly to a clean
    split, then two lobes drift apart. No instantaneous topology jump; the neck collapses
    over many layers (physically real Y bifurcation)."""
    rng = np.random.default_rng(J.SEED + 33)
    layers = []
    split_at = 60
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        if k < split_at:
            # waist shrinks smoothly to ~0 right at split_at
            frac = k / split_at
            waist = 6.0 * (1.0 - frac) ** 1.3 + 0.05
            sep = 15 + 8 * frac
            loops.append((0, A._peanut(0, 0, sep, 6.8, waist, n=340)))
        else:
            f = (k - split_at) / max(1, nlayers - split_at)
            sep = 23 + 10 * f
            loops.append((1, _circle(-sep / 2, 0, 6.9)))
            loops.append((2, _circle(+sep / 2, 0, 6.9)))
        if k % 2:
            loops = loops[::-1]
        layers.append([J._loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


def part_pure_circle(nlayers=120):
    """Control: a single circle that SLOWLY shrinks (one N change region). The candidate
    should be near-zero except at the unavoidable N-change layer."""
    rng = np.random.default_rng(J.SEED + 34)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        r = 14.0 - 4.0 * z          # slowly shrinks: L from ~88 to ~63, crosses N=22->?
        layers.append([J._loop(_circle(0, 0, r), 0, rng)])
    return layers


def dump(layers, label, around=None):
    pack = spec["prepare"](layers)
    state = pack["state"]
    track_of = pack["track_of"]
    print(f"\n==== {label} ====")
    # per-pair interlock
    pairs = []
    for li in range(len(layers) - 1):
        Aisl = J._island_loops(layers[li]); Bisl = J._island_loops(layers[li + 1])
        worst = 0.0
        for iid, la in Aisl.items():
            if iid not in Bisl: continue
            lb = Bisl[iid]
            za = spec["field"](la, li, "nested", pack)
            zb = spec["field"](lb, li + 1, "nested", pack)
            j, dist = J._match_pts(la["xy"], lb["xy"], 0.5)
            ok = dist <= 0.5
            if ok.any():
                e = za[ok] + zb[j[ok]]
                worst = max(worst, 100.0 * math.sqrt(np.mean(e ** 2)) / J.AMP)
        pairs.append((li, worst))
    pairs_sorted = sorted(pairs, key=lambda t: -t[1])
    print("  worst pairs:", [(li, round(w, 1)) for li, w in pairs_sorted[:6]])
    # detail around the worst (and any user-requested layers)
    focus = set([pairs_sorted[0][0]])
    if around: focus |= set(around)
    for li in sorted(focus):
        for d in (0, 1):
            L = li + d
            if L >= len(state): continue
            recs = state[L]
            info = []
            for tid, rec in sorted(recs.items()):
                info.append(f"trk{tid}(iid{rec['island_id']} N={rec['N']} s0={rec['s0']:.2f} "
                            f"reseed={int(rec['reseed'])})")
            print(f"   layer {L:3d}: " + " | ".join(info))


def main():
    dump(part_pure_circle(), "pure shrinking circle (control)")
    dump(part_split_smooth(), "SMOOTH Y split", around=[58, 59, 60, 61, 62])


if __name__ == "__main__":
    main()
