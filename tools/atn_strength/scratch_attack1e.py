#!/usr/bin/env python3
"""ATTACK 1e: confirm the N-change break is (a) physically realistic and gentle, not a
contrived violent oscillation, and (b) that the harness GATE PASSES the part anyway (the
candidate 'gets away with it' because every aggregate metric dilutes the 3 bad planes).

We build a realistic FLUTED BALUSTER / velocity-stack bell: radius varies smoothly and
slowly (one gentle bulge then neck), crossing exactly ONE N boundary up and ONE back down
-> just 2 N-change planes in 130 layers. Then we run the harness's OWN gate logic on it."""
import math
import numpy as np

import validate_weave2 as J
import scratch_attack1 as A

name = "island_prop"
spec = J._METHODS[name]
LAM = J.LAMBDA


def _circle(cx, cy, r, n=None):
    return A._circle(cx, cy, r, n)


def part_baluster(nlayers=130):
    """Smooth single bulge: r goes 7 -> 12 -> 7 over the whole height (one slow breath).
    L = 2*pi*r goes ~44 -> ~75 -> ~44. With lambda=4: at r=7 N~11 (lam~4.0); holding N=11
    as r climbs, lam_if_keep = L/11 grows; at r=12 L~75.4, lam=6.85 > 1.6*4=6.4 -> ONE
    re-round up. Coming back down forces ONE re-round back. Just 2 events, very gentle,
    entirely realistic (a vase / bell). Everything else is pristine."""
    rng = np.random.default_rng(J.SEED + 51)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        r = 7.0 + 5.0 * math.sin(math.pi * z)   # 7 -> 12 -> 7, one smooth bulge
        layers.append([J._loop(_circle(0, 0, r), 0, rng)])
    return layers


def worst_pairs(layers, pack):
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
                pairs.append((100.0*math.sqrt(np.mean(e**2))/J.AMP,
                              100.0*np.max(np.abs(e))/J.AMP, li, iid))
    pairs.sort(reverse=True)
    return pairs


def main():
    layers = part_baluster()
    pack = spec["prepare"](layers)
    # N changes
    Nlog = []
    prevN = {}
    for li, recs in enumerate(pack["state"]):
        for tid, rec in recs.items():
            if tid in prevN and prevN[tid] != rec["N"]:
                Nlog.append((li, prevN[tid], rec["N"]))
            prevN[tid] = rec["N"]
    pairs = worst_pairs(layers, pack)

    # whole-part metrics + emulate the harness gate (single-island part)
    il = J.interlock_rms_pct(layers, spec, pack)
    isl = J.per_island_ok(layers, spec, pack)
    lam = J.lambda_cov(layers, spec, pack)
    b, t, climb = J.top_lambda_cov(layers, spec, pack)
    amp = J.amp_cov(layers, spec, pack)
    clo = J.closure_err(layers, spec, pack)
    cov, mod = J.coverage_modulation(layers, spec, pack)

    print("==== REALISTIC FLUTED BALUSTER / BELL (r: 7->12->7, one gentle breath) ====")
    print(f"  N-change planes: {Nlog}  (only {len(Nlog)} in {len(layers)} layers)")
    print(f"\n  -- WHOLE-PART aggregate metrics (what the gate sees) --")
    print(f"     interlock = {il:6.1f}%   (gate needs <30)  -> {'PASS' if il<30 else 'FAIL'}")
    print(f"     per_island= {'yes' if isl else 'NO'}        (gate needs yes)-> {'PASS' if isl else 'FAIL'}")
    print(f"     lam_cov   = {lam:6.2f}    (gate needs <0.25)-> {'PASS' if lam<0.25 else 'FAIL'}")
    print(f"     lam_climb = {climb:6.2f}    (gate needs <0.15)-> {'PASS' if climb<0.15 else 'FAIL'}")
    print(f"     amp_cov   = {amp:6.2f}    (gate needs <0.20)-> {'PASS' if amp<0.20 else 'FAIL'}")
    print(f"     closure   = {clo:6.2f}    (gate needs <0.20)-> {'PASS' if clo<0.20 else 'FAIL'}")
    print(f"     coverage  = {cov:6.2f}  modulation={mod:.2f}")
    gate_ok = (il<30 and isl and lam<0.25 and climb<0.15 and amp<0.20 and clo<0.20
               and cov>0.40 and mod>0.45)
    print(f"\n  >>> HARNESS GATE VERDICT for this part: {'PASS' if gate_ok else 'FAIL'} <<<")
    print(f"\n  -- but the WORST ADJACENT WELD PLANES (the physical reality) --")
    for r in pairs[:6]:
        tag = "  <== N-CHANGE PLANE (catastrophic)" if any(nl[0]==r[2]+1 for nl in Nlog) else ""
        print(f"     layer {r[2]:3d}->{r[2]+1:3d}: interlock RMS={r[0]:6.1f}%  max={r[1]:6.1f}%{tag}")
    print(f"\n  WORST plane interlock = {pairs[0][0]:.1f}% RMS, {pairs[0][1]:.1f}% max"
          f"  (141% = random; >100% = ANTI-interlock, peak-over-peak)")
    print("\n  INTERPRETATION: at the 2 N-change planes the weave inverts (peak sits over")
    print("  peak, not trough) -> ZERO and even NEGATIVE Z interlock exactly there. The part")
    print("  passes every aggregate gate because 2 bad planes in 130 dilute to ~17% RMS.")


if __name__ == "__main__":
    main()
