#!/usr/bin/env python3
"""Measure REAL woven-wall registration straight from a sliced gcode (closes the
twin->binary gap from WOVEN_REDESIGN_SPEC.md step 8).

For each pair of adjacent layers it matches every wall point on layer N to the nearest
wall point on layer N+1 (within eps) and reports the ADJACENT-LAYER INTERLOCK:
  off = Z - layer_print_z   (the woven sub-layer Z modulation)
  nested weave => layer N and N+1 are antiphase, so a REGISTERED weave has
  off_N(p) + off_{N+1}(p) ~ 0.  interlock_rms = RMS(off_N+off_{N+1}) / amp.
  0% = perfect peak-over-trough interlock; ~141% = random; >100% = anti-interlock
  (peak-over-peak = a horizontal DE-BOND plane, the failure the spec warns about).

    python weave_measure.py <plate.gcode>
"""
import sys, re, numpy as np

import sys as _sys
EPS = float(_sys.argv[2]) if len(_sys.argv) > 2 else 0.5   # mm, adjacent-layer match radius
SUB = 2500         # subsample wall points per layer for the metric

def grid_nearest(P, Q, eps):
    """For each point in P, nearest point in Q within eps. Returns (idxP, idxQ)."""
    if len(P) == 0 or len(Q) == 0:
        return np.empty(0, int), np.empty(0, int)
    inv = 1.0 / eps
    cell = {}
    qk = np.floor(Q[:, :2] * inv).astype(np.int64)
    for i, (cx, cy) in enumerate(qk):
        cell.setdefault((cx, cy), []).append(i)
    pi, qi = [], []
    pk = np.floor(P[:, :2] * inv).astype(np.int64)
    for i, (cx, cy) in enumerate(pk):
        best, bj = eps * eps, -1
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in cell.get((cx + dx, cy + dy), ()):
                    d = (P[i, 0] - Q[j, 0]) ** 2 + (P[i, 1] - Q[j, 1]) ** 2
                    if d < best:
                        best, bj = d, j
        if bj >= 0:
            pi.append(i); qi.append(bj)
    return np.asarray(pi, int), np.asarray(qi, int)

def main():
    path = sys.argv[1]
    rx_z   = re.compile(r"^;Z:([0-9.]+)")
    rx_typ = re.compile(r"^;TYPE:(.+)")
    layers = []                 # (print_z, Nx3 array of x,y,off)
    cur, print_z, in_wall = [], None, False
    x = y = z = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = rx_z.match(line)
            if m:
                if cur and print_z is not None:
                    layers.append((print_z, np.asarray(cur, float)))
                cur, print_z = [], float(m.group(1)); continue
            m = rx_typ.match(line)
            if m:
                in_wall = "wall" in m.group(1).lower(); continue
            if in_wall and line.startswith("G1 "):
                xx = yy = zz = None; e = False
                for tok in line.split():
                    c = tok[0]
                    if c == 'X': xx = float(tok[1:])
                    elif c == 'Y': yy = float(tok[1:])
                    elif c == 'Z': zz = float(tok[1:])
                    elif c == 'E':
                        try: e = float(tok[1:]) > 0
                        except ValueError: e = False
                if xx is not None: x = xx
                if yy is not None: y = yy
                if zz is not None: z = zz
                if e and x is not None and y is not None and z is not None and print_z is not None:
                    cur.append((x, y, z))          # raw z; centred per-layer below
    if cur and print_z is not None:
        layers.append((print_z, np.asarray(cur, float)))

    # Centre each layer's wall Z on its OWN mean (robust to precise_z_height / base
    # convention): the woven offset is a symmetric sinusoid, so mean(wall z) == true base.
    for _, L in layers:
        if len(L):
            L[:, 2] -= L[:, 2].mean()

    alloff = np.concatenate([L[:, 2] for _, L in layers if len(L)]) if layers else np.zeros(1)
    amp = max(np.percentile(np.abs(alloff), 99), 1e-4)
    print(f"layers parsed: {len(layers)}   measured amp ~ {amp*1000:.0f} um   "
          f"mean|off|/amp = {np.mean(np.abs(alloff))/amp:.2f} (weave present if >~0.4)")

    rng = np.random.default_rng(0)
    rms_by_h, debond = [], 0
    active_rms = []                              # pairs where the weave is at full amplitude
    debond_lowamp = 0                            # de-bonds that sit in a faded (taper) region
    layer_peak = []                              # per-layer wave peak (95th pct |off|), full-amp layers
    for k in range(len(layers) - 1):
        z0, A = layers[k]; z1, B = layers[k + 1]
        if len(A) < 20 or len(B) < 20:
            continue
        amp_a = np.mean(np.abs(A[:, 2])) / amp   # local weave strength (0 in taper, ~0.6 full)
        amp_b = np.mean(np.abs(B[:, 2])) / amp
        if amp_a > 0.4:                          # amplitude consistency: peak height per full-amp layer
            layer_peak.append(np.percentile(np.abs(A[:, 2]), 95))
        if len(A) > SUB: A = A[rng.choice(len(A), SUB, replace=False)]
        pi, qi = grid_nearest(A, B, EPS)
        if len(pi) < 20:
            continue
        s = A[pi, 2] + B[qi, 2]                  # registered nested weave => ~0
        rms = float(np.sqrt(np.mean(s * s)) / amp)
        rms_by_h.append((z1, rms, len(pi)))
        active = min(amp_a, amp_b) > 0.4         # both layers actively woven (not taper)
        if active:
            active_rms.append(rms)
        if rms > 0.80:
            debond += 1
            if not active:
                debond_lowamp += 1
    if not rms_by_h:
        print("no adjacent-layer pairs measured (no wall points?)"); return
    r = np.array([v[1] for v in rms_by_h])
    ar = np.array(active_rms)
    print(f"\nADJACENT-LAYER INTERLOCK RMS (% amp, 0=ideal nested, ~141=random, >100=de-bond):")
    print(f"  pairs measured : {len(r)}")
    print(f"  median         : {100*np.median(r):.1f}%")
    print(f"  90th pct       : {100*np.percentile(r,90):.1f}%")
    print(f"  worst          : {100*np.max(r):.1f}%  (at Z={rms_by_h[int(np.argmax(r))][0]:.2f})")
    print(f"  DE-BOND planes (>80%): {debond}  ({debond_lowamp} of them in faded/taper "
          f"layers, {debond-debond_lowamp} in fully-woven layers <-- this is what matters)")
    print(f"\nACTIVE-WEAVE-ONLY (both layers at full amplitude, taper excluded): n={len(ar)}")
    if len(ar):
        print(f"  median {100*np.median(ar):.1f}%   90th {100*np.percentile(ar,90):.1f}%   "
              f"worst {100*np.max(ar):.1f}%   de-bonds {int((ar>0.80).sum())}")
    if len(layer_peak):
        lp = np.array(layer_peak)
        print(f"\nAMPLITUDE CONSISTENCY (wave peak height across full-amp layers): n={len(lp)}")
        print(f"  mean peak {1000*np.mean(lp):.1f}um   CoV {100*np.std(lp)/np.mean(lp):.1f}%  "
              f"(lower=more consistent)   range {1000*np.min(lp):.0f}-{1000*np.max(lp):.0f}um")
    zs = np.array([v[0] for v in rms_by_h])
    print(f"\n  interlock vs height (12 bands, % amp):")
    edges = np.linspace(zs.min(), zs.max(), 13)
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = r[(zs >= lo) & (zs < hi)]
        if len(sel): print(f"    Z {lo:6.1f}-{hi:6.1f} : {100*np.mean(sel):5.1f}%  (n={len(sel)})")
    print("\nRESULT:", "PASS - registered weave, no de-bond planes"
          if (np.median(r) < 0.35 and debond == 0) else
          "REVIEW - " + (f"{debond} de-bond planes" if debond else f"median {100*np.median(r):.0f}%"))

if __name__ == "__main__":
    main()
