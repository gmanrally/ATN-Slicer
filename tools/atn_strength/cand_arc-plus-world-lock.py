#!/usr/bin/env python3
"""Candidate weave method: ARC-PLUS-WORLD-LOCK.

A HYBRID phase field: arc-length gives within-layer uniform wavelength and exact
closure on ANY loop; a coarse, slowly-varying WORLD reference bounds the long-range
phase so it cannot wander between adjacent layers (the headline strength metric).

Why a hybrid, and what the world lock actually locks
----------------------------------------------------
Three independent things can de-register adjacent layers in an arc-length weave; the
world reference pins each one to a physically-stable quantity rather than to a
slicer-internal one:

  (1) PHASE ORIGIN.  phase = 2*pi*n*(s - s0)/L needs an origin s0. If s0 follows the
      slicer seam it jumps randomly; if it follows a per-loop intrinsic landmark it
      drifts as the loop morphs.  LOCK: s0 = arc-length of the loop's +x-axis crossing
      about its OWN centroid, found by a CONTINUOUS (sub-vertex, interpolated) zero
      crossing.  Adjacent layers share almost the same centroid and outline, so this
      sits at almost the same physical (x,y) on layer N and N+1.  Blend knob
      `world_lock` in [0,1] trades this lock (1.0) against a raw intrinsic landmark
      (0.0) -- swept and reported.

  (2) WAVE COUNT n.  n = round(L/lambda) STEPS by 1 whenever L crosses a half-integer
      multiple of lambda.  A bare step re-phases the WHOLE loop -> that layer-pair's
      interlock collapses to ~random (measured: a single n-step pair scores ~100%).
      LOCK (developable loops): use the CONTINUOUS wave count nu = L/lambda for the
      field (registers exactly between adjacent layers since L barely changes), and
      absorb the closure deficit (n - nu) as a SMOOTH raised-cosine phase ramp so the
      seam still completes an integer number of waves (req 4) without a frequency step.
      The deficit per loop is <= 0.5 wave and spread over the whole loop, so it perturbs
      registration far less than a hard step.

  (3) NON-DEVELOPABLE FEATURES (surfaces of revolution: the dome, the velocity-stack
      trumpets).  Here the perimeter shrinks fast: keeping a fixed WAVELENGTH forces nu
      to drop ~0.1-0.8 wave per layer, and the far side of the loop then de-registers no
      matter how clean the origin is (the non-developable-surface fact, made concrete).
      The ONLY field that registers a fast-collapsing SoR is a fixed WAVE COUNT about a
      fixed axis (n*theta) -- which does NOT fan on a round loop because theta is
      uniform there.  LOCK (round loops): n*theta about the loop centroid with n FIXED
      for the whole feature (taken at its widest layer, and shared across the feature by
      per-island tracking in prepare()).  This is exactly the engine's round-loop path.

The circularity 4*pi*A/L^2 selects between the two branches with a smooth ramp
(developable arc-length below ~0.85, fixed-count radial above ~0.97, blended between).
No world SINUSOID is ever added to the field, so it never beats; the world reference
only places the origin and fixes the count.

PER-ISLAND + robustness
-----------------------
prepare() tracks each island across height by id (the harness exposes island_id; the
shipped engine recovers the same grouping by centroid) and stores, per feature: a
fixed radial wave count (from its widest layer) and a windowed-smoothed developable
wave count.  Every loop is anchored from its OWN centroid and L, so separate islands
never share a phase reference -> no cross-feature contamination.  Birth/death/merge of
a feature just means that loop re-anchors from its own stable centroid; there is no
global recurrence to scramble, so it cannot accumulate fanning the way a propagation
matcher can.

HONEST WEAKNESS: a GENUINELY TWISTING wall (real geometric rotation with height) cannot
be registered by any world-keyed reference -- the +x crossing and the radial axis both
rotate with the part while the field does not.  Reported separately on the twist part.
"""
import math
import numpy as np
import validate_weave2 as J

LAMBDA = J.LAMBDA
AMP    = J.AMP

# Origin lock blend: 1.0 = phase origin fully slaved to the coarse world reference (the
# +x crossing); 0.0 = origin from a raw per-loop intrinsic landmark (drifts as the loop
# morphs). Default full lock; swept in main().
WORLD_LOCK = 1.0

# Circularity ramp: below CIRC_LO use developable (arc-length) branch; above CIRC_HI use
# the fixed-count radial branch; blend between. A circle is 1.0; a 3:1 ellipse ~0.69.
CIRC_LO = 0.85
CIRC_HI = 0.97

# Window (layers) over which the developable wave count is smoothed so it steps rarely.
NSMOOTH_WIN = 9


# ----------------------------------------------------------------------------- origin
def _origin_arclen(xy, s, L, centroid, world_lock):
    """Phase-origin arc-length s0: the loop's +x-axis crossing about its own centroid,
    interpolated to sub-vertex so it does not jitter by a whole vertex between layers.
    With world_lock<1 we blend toward the raw min-|theta| vertex (which snaps)."""
    th = np.arctan2(xy[:, 1] - centroid[1], xy[:, 0] - centroid[0])
    s_world = _interp_plus_x_crossing(th, s, L)
    if world_lock < 1.0:
        s_intr = float(s[int(np.argmin(np.abs(th)))])
        s_world = _circ_blend(s_intr, s_world, world_lock, L)
    return s_world


def _interp_plus_x_crossing(th, s, L):
    """Arc-length where theta (angle about centroid) crosses 0 ascending (+x axis),
    interpolated. Robust to loop winding; falls back to the min-|theta| vertex."""
    n = len(th)
    thn = np.roll(th, -1)
    asc = (th <= 0.0) & (thn > 0.0) & (np.abs(thn - th) < math.pi)
    idx = np.where(asc)[0]
    if len(idx) == 0:
        dsc = (th > 0.0) & (thn <= 0.0) & (np.abs(thn - th) < math.pi)
        idx = np.where(dsc)[0]
        if len(idx) == 0:
            return float(s[int(np.argmin(np.abs(th)))])
    i = int(idx[0])
    j = (i + 1) % n
    denom = th[j] - th[i]
    t = (0.0 - th[i]) / denom if abs(denom) > 1e-12 else 0.0
    t = min(1.0, max(0.0, t))
    si = s[i]
    sj = s[j] if j != 0 else L
    return float(si + (sj - si) * t)


def _circ_blend(a, b, w, L):
    """Blend two arc-lengths on the circle of circumference L (short way round)."""
    d = (b - a + L) % L
    if d > L / 2:
        d -= L
    return (a + w * d) % L


# ----------------------------------------------------------------------------- prepare
def _prepare(layers):
    """Per-feature bookkeeping (the world lock for the WAVE COUNT): track each island
    across height and store (a) a windowed-smoothed developable wave count nmap and
    (b) a single FIXED radial wave count per feature (from its widest layer) plus a
    smoothed circularity. Stateless per point thereafter."""
    series = {}
    for li, layer in enumerate(layers):
        best = {}
        for lp in layer:
            iid = lp["island_id"]
            if iid not in best or lp["L"] > best[iid]["L"]:
                best[iid] = lp
        for iid, lp in best.items():
            circ = 4.0 * math.pi * lp["area"] / (lp["L"] * lp["L"] + 1e-9)
            series.setdefault(iid, {})[li] = (lp["L"], circ)

    nmap, cmap, nfix = {}, {}, {}
    half = NSMOOTH_WIN // 2
    for iid, d in series.items():
        lis = sorted(d)
        Lmax = max(d[j][0] for j in lis)
        nfix[iid] = max(1, int(round(Lmax / LAMBDA)))  # fixed radial count for the feature
        for li in lis:
            win = [j for j in lis if abs(j - li) <= half]
            Lsm = float(np.mean([d[j][0] for j in win]))
            csm = float(np.mean([d[j][1] for j in win]))
            nmap[(li, iid)] = max(1, int(round(Lsm / LAMBDA)))
            cmap[(li, iid)] = csm
    return dict(nmap=nmap, cmap=cmap, nfix=nfix, world_lock=WORLD_LOCK)


# ----------------------------------------------------------------------------- field
def _field(loop, li, mode, state):
    xy, s, L = loop["xy"], loop["s"], loop["L"]
    cen = loop["centroid"]
    iid = loop["island_id"]
    sign = J._sign(li, mode)

    if state is None:  # defensive: harness always calls prepare, but stay safe
        state = dict(nmap={}, cmap={}, nfix={}, world_lock=WORLD_LOCK)
    wl = state.get("world_lock", WORLD_LOCK)

    circ = state["cmap"].get((li, iid), 4.0 * math.pi * loop["area"] / (L * L + 1e-9))
    w = min(1.0, max(0.0, (circ - CIRC_LO) / (CIRC_HI - CIRC_LO)))  # 0=arc, 1=radial

    th = np.arctan2(xy[:, 1] - cen[1], xy[:, 0] - cen[0])

    # --- radial branch: fixed wave count about the feature axis (registers a SoR) ---
    if w >= 1.0:
        nf = state["nfix"].get(iid, max(1, int(round(L / LAMBDA))))
        return AMP * sign * np.sin(nf * th)

    # --- developable branch: world-locked arc-length, continuous count + smooth closure ---
    n = state["nmap"].get((li, iid), max(1, int(round(L / LAMBDA))))
    s0 = _origin_arclen(xy, s, L, cen, wl)
    u = np.mod(s - s0, L) / L                       # 0..1 around the loop from the origin
    nu = L / LAMBDA                                 # continuous wave count -> exact adj. reg.
    frac = n - nu                                   # closure deficit (<= 0.5 wave) to absorb
    corr = u - np.sin(2.0 * math.pi * u) / (2.0 * math.pi)   # smooth, 0->1, integrates the bump
    ph_arc = 2.0 * math.pi * (nu * u + frac * corr)

    if w <= 0.0:
        return AMP * sign * np.sin(ph_arc)

    # --- blend zone: combine on the phase circle to avoid wrap-averaging artifacts ---
    nf = state["nfix"].get(iid, n)
    ph_ang = nf * th
    z = (1.0 - w) * np.exp(1j * ph_arc) + w * np.exp(1j * ph_ang)
    return AMP * sign * np.sin(np.angle(z))


def register(name="arc-plus-world-lock", world_lock=None):
    global WORLD_LOCK
    if world_lock is not None:
        WORLD_LOCK = world_lock
    J.register_method(name, dict(prepare=_prepare, field=_field))
    return name


# ----------------------------------------------------------------------------- run it
def _build_parts():
    parts_cache = {}
    for pname, (fac, is_mi) in J.PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)
    twist_layers = J.TWIST_PART[1]()
    J._EVENTS["twist"] = J.prop_event_counts(twist_layers)
    return parts_cache, twist_layers


def _fmt(v, p=2):
    if isinstance(v, bool):
        return "yes" if v else "NO"
    if v != v:
        return "nan"
    return f"{v:.{p}f}"


def main():
    parts_cache, twist_layers = _build_parts()

    name = register("arc-plus-world-lock", world_lock=1.0)
    res = J.score_method(name, parts_cache)
    parts = list(J.PARTS.keys())

    print("=== arc-plus-world-lock : per-part metrics (world_lock=1.0) ===")
    hdr = (f"{'part':13s} {'interlk':>8s} {'il@evt':>7s} {'lam_cov':>8s} "
           f"{'lamclmb':>8s} {'amp_cov':>8s} {'closure':>8s} {'island':>7s} "
           f"{'reseed':>7s} {'misbnd':>7s}")
    print(hdr)
    print("-" * len(hdr))
    for p in parts:
        r = res[p]
        print(f"{p:13s} {_fmt(r['interlock'],1):>8s} {_fmt(r['il_event'],1):>7s} "
              f"{_fmt(r['lam'],3):>8s} {_fmt(r['lam_climb'],3):>8s} "
              f"{_fmt(r['amp'],3):>8s} {_fmt(r['clo'],3):>8s} {_fmt(r['isl']):>7s} "
              f"{r['reseeds']:>7d} {r['misbinds']:>7d}")
    g = res["_global"]
    print(f"\nglobal: coverage={g['coverage']:.3f}  modulation={g['modulation']:.3f}")
    tw = J.score_twist(name, twist_layers)
    print(f"twist (separate; world-keyed lock CANNOT register a real twist): "
          f"interlock_rms_pct={_fmt(tw,1)}")

    # ---- replicate the harness PASS gate thresholds (we do NOT modify the harness) ----
    GATE_PARTS = ("multi_island", "sharp_box", "elongated", "dome")
    reasons = []
    for p in GATE_PARTS:
        r = res[p]
        if not (r["interlock"] == r["interlock"] and r["interlock"] < 30.0):
            reasons.append(f"{p}.interlock>=30")
        has_events = (r["reseeds"] + r["misbinds"]) > 0
        if has_events and r["il_event"] == r["il_event"] and r["il_event"] >= 35.0:
            reasons.append(f"{p}.il@event>=35")
        if not (r["lam"] == r["lam"] and r["lam"] < 0.25):
            reasons.append(f"{p}.lam_cov>=0.25")
        if not (r["lam_climb"] == r["lam_climb"] and r["lam_climb"] < 0.15):
            reasons.append(f"{p}.lam_climb>=0.15")
        if not (r["amp"] == r["amp"] and r["amp"] < 0.20):
            reasons.append(f"{p}.amp_cov>=0.20")
        if not (r["clo"] == r["clo"] and r["clo"] < 0.20):
            reasons.append(f"{p}.closure>=0.20")
        if not r["isl"]:
            reasons.append(f"{p}.per_island_NO")
    if not (tw == tw and tw < 30.0):
        reasons.append(f"twist={_fmt(tw,1)}>=30")
    if not (g["coverage"] > 0.40 and g["modulation"] > 0.45):
        reasons.append("not_a_real_weave")
    print(f"\nPASS GATE (harness thresholds): {'PASS' if not reasons else 'FAIL'}"
          + ("" if not reasons else "  <- " + "; ".join(reasons)))

    # ---- sensitivity sweep: world_lock (origin lock) ----
    print("\n=== SENSITIVITY: world_lock (origin lock) sweep — interlock RMS % (lower=better) ===")
    print(f"{'lock':>5s} " + "".join(f"{p:>14s}" for p in parts) + f"{'twist':>9s}")
    for wl in (0.0, 0.25, 0.5, 0.75, 1.0):
        nm = f"awl_lock_{wl}"
        register(nm, world_lock=wl)
        rr = J.score_method(nm, parts_cache)
        tww = J.score_twist(nm, twist_layers)
        row = f"{wl:>5.2f} " + "".join(f"{_fmt(rr[p]['interlock'],1):>14s}" for p in parts)
        print(row + f"{_fmt(tww,1):>9s}")
    register("arc-plus-world-lock", world_lock=1.0)

    # ---- sensitivity sweep: circularity blend threshold (round-branch selectivity) ----
    print("\n=== SENSITIVITY: CIRC_HI radial-branch threshold — interlock RMS % ===")
    global CIRC_HI, CIRC_LO
    base_lo, base_hi = CIRC_LO, CIRC_HI
    print(f"{'CIRC_HI':>8s} " + "".join(f"{p:>14s}" for p in parts))
    for hi in (0.93, 0.95, 0.97, 0.99):
        CIRC_HI = hi
        CIRC_LO = min(base_lo, hi - 0.08)
        nm = f"awl_circ_{hi}"
        register(nm, world_lock=1.0)
        rr = J.score_method(nm, parts_cache)
        print(f"{hi:>8.2f} " + "".join(f"{_fmt(rr[p]['interlock'],1):>14s}" for p in parts))
    CIRC_LO, CIRC_HI = base_lo, base_hi
    register("arc-plus-world-lock", world_lock=1.0)

    return res, g, tw, reasons


if __name__ == "__main__":
    main()
