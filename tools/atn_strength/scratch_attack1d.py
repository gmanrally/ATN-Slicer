#!/usr/bin/env python3
"""ATTACK 1d: two sharper probes.
 (1) N-BOUNDARY OSCILLATION: a single round loop whose perimeter L breathes back and forth
     ACROSS a round(L/lambda) boundary. The hysteresis band [0.62,1.6]*lambda is meant to
     stop N chatter, but a part can be tuned so that when N is held, lam_if_keep leaves the
     band, forcing a re-round; once re-rounded, the reverse breath forces it back. We make L
     oscillate with an amplitude chosen to repeatedly exit the band -> repeated N changes,
     each a ~100% interlock layer that the whole-loop average dilutes.
 (2) CUSP zoom: the cusp part already showed 32.6% worst-pair / 81% max with NO events and
     N held. That means the arc-length FRAME ITSELF distorts adjacent layers: where the wall
     grows a cusp, equal arc-length s maps to very different physical (x,y) between layers,
     so phi(s-s0) at a physical point differs layer-to-layer EVEN with N and s0 'aligned'.
     We quantify how bad the worst cusp pair really is and whether per_island catches it."""
import math
import numpy as np

import validate_weave2 as J
import scratch_attack1 as A

name = "island_prop"
spec = J._METHODS[name]
LAM = J.LAMBDA


def _circle(cx, cy, r, n=None):
    return A._circle(cx, cy, r, n)


def part_Nbreath(nlayers=140):
    """Round loop whose circumference oscillates across an N boundary. lambda=4mm; pick a
    mean radius so L/lambda sits near an integer+0.5 and oscillate +-enough that round()
    flips. With the band hold, N flips only when the IMPLIED wavelength leaves the band; we
    tune the swing so it does, every cycle."""
    rng = np.random.default_rng(J.SEED + 41)
    layers = []
    # target: L swings so that L/N (held) crosses 1.60*lambda or 0.62*lambda.
    # mean r ~ 7 -> L ~ 44 -> N ~ 11, lambda 4. To push lam_if_keep>1.6*4=6.4 with N=11
    # need L>70.4 (r>11.2). To push <0.62*4=2.48 need L<27 (r<4.3). So oscillate r in
    # [4.0, 12.0] -> forces re-rounds at both ends, repeatedly.
    for k in range(nlayers):
        z = k / nlayers
        r = 8.0 + 4.0 * math.sin(2.0 * math.pi * z * 3)  # 3 full breaths over the stack
        layers.append([J._loop(_circle(0, 0, max(2.5, r)), 0, rng)])
    return layers


def part_cusp_strong(nlayers=120):
    """A teardrop with a deeper, persistent cusp that GROWS monotonically (a forming spike),
    so the arc-frame distortion is sustained, not transient."""
    rng = np.random.default_rng(J.SEED + 42)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        tail = 22.0 * z          # grows 0 -> 22 monotonically
        td = A._teardrop(0, 0, 12.0, tail, n=320)
        layers.append([J._loop(td, 0, rng)])
    return layers


def analyse(layers, label, full=False):
    pack = spec["prepare"](layers)
    state = pack["state"]
    # detect N changes per track
    Nchanges = 0
    Nlog = []
    prevN = {}
    for li, recs in enumerate(state):
        for tid, rec in recs.items():
            if tid in prevN and prevN[tid] != rec["N"]:
                Nchanges += 1
                Nlog.append((li, tid, prevN[tid], rec["N"]))
            prevN[tid] = rec["N"]
    pairs = []
    for li in range(len(layers) - 1):
        Aisl = J._island_loops(layers[li]); Bisl = J._island_loops(layers[li + 1])
        for iid, la in Aisl.items():
            if iid not in Bisl: continue
            lb = Bisl[iid]
            za = spec["field"](la, li, "nested", pack)
            zb = spec["field"](lb, li + 1, "nested", pack)
            j, dist = J._match_pts(la["xy"], lb["xy"], 0.5)
            ok = dist <= 0.5
            if ok.any():
                e = za[ok] + zb[j[ok]]
                rms = 100.0 * math.sqrt(np.mean(e ** 2)) / J.AMP
                mx = 100.0 * np.max(np.abs(e)) / J.AMP
                pairs.append((rms, mx, li, iid))
    pairs.sort(reverse=True)
    # whole-part metrics
    il = J.interlock_rms_pct(layers, spec, pack)
    isl = J.per_island_ok(layers, spec, pack)
    lam = J.lambda_cov(layers, spec, pack)
    clo = J.closure_err(layers, spec, pack)
    print(f"\n==== {label} ====")
    print(f"  whole-part: interlock={il:.1f}%  per_island={'yes' if isl else 'NO'}  "
          f"lam_cov={lam:.2f}  closure={clo:.2f}   N-changes={Nchanges}")
    print(f"  N-change layers: {Nlog[:12]}")
    print(f"  worst pairs (RMS%,max%,layer,island):")
    for r in pairs[:8]:
        tag = "  <-- N-change here" if any(nl[0] == r[2] + 1 or nl[0] == r[2] for nl in Nlog) else ""
        print(f"    RMS={r[0]:6.1f} max={r[1]:6.1f} layer {r[2]:3d}->{r[2]+1:3d} isl {r[3]}{tag}")
    print(f"  WORST pair RMS = {pairs[0][0]:.1f}%   WORST pair MAX = {pairs[0][1]:.1f}%")


def main():
    analyse(part_Nbreath(), "N-BOUNDARY OSCILLATION (forced repeated N changes)")
    analyse(part_cusp_strong(), "CUSP GROWING (arc-frame distortion)")


if __name__ == "__main__":
    main()
