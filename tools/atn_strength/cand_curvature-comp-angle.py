#!/usr/bin/env python3
r"""Candidate phase-field method: CURVATURE-COMP-ANGLE.

STRATEGY (control / sanity baseline)
------------------------------------
The naive ANGLE field  phi = N*theta  (theta measured about the loop centroid)
gives PERFECT vertical registration on any geometry -- theta is a position-keyed
intrinsic coordinate that changes only as fast as the shape morphs -- but it FANS
on non-circular loops: a fixed d(theta) step sweeps a long arc where the radius r
is large and a short arc where r is small, so the wavelength along the wall is
proportional to r and bunches/fans by the loop's r_max/r_min ratio (3:1 on the
elongated airbox body).

The fix this strategy proposes is to CORRECT the angular step by the local radius:

    phi(point) = 2*pi*N * g(point) / G ,    g = INTEGRAL_{theta0}^{theta} r dtheta

i.e. accumulate  r*dtheta  instead of  dtheta .  Because the arc-length element of
a curve given in polar form about the centroid is  ds = sqrt(r^2 + (dr/dtheta)^2) dtheta,
weighting d(theta) by r (== r/r_mean up to the constant 1/G normalisation) makes the
accumulated coordinate track  r dtheta , which for a circle is EXACTLY arc length
(r*dtheta = ds) and for a mildly non-circular loop is a close approximation of it.
So this method is, by construction, a bridge between the pure angle field and the
pure arc-length field -- it CONFIRMS the equivalence to arc length while keeping the
angle field's position-keyed seam (theta0 = a fixed world ray from the centroid),
which is what buys the adjacent-layer registration.

Properties wired in for the judge's five requirements:

  (1) ADJACENT-LAYER registration (headline): the phase origin is the centroid ray
      pointing at a FIXED world direction (+x). theta and the swept coordinate g/G of
      a physical (x,y) change only as fast as the centroid + outline morph between
      layer N and N+1, so phase_N(x,y) ~ phase_{N+1}(x,y); the nested parity sign then
      realises the controlled half-wave stagger (peak over trough). This is intrinsic
      per-loop, NOT a world scalar field, so it does not beat.
  (2) Uniform wavelength: g accumulates r*dtheta ~ arc length, cancelling the angle
      field's r-proportional fanning.
  (3) Uniform amplitude: amplitude is a constant AMP * sin(phi); no spatial envelope
      -> no beating.
  (4) Closure (integer waves): phi = 2*pi*N*g/G with N = round(perimeter/lambda) and
      g closing at exactly G after one loop -> sin completes N whole waves, no seam jump.
  (5) Per-island + robustness: every quantity (centroid, theta0 ray, r, g, N) is
      computed FROM THE LOOP ITSELF. There is no cross-loop state, so islands are
      independent by construction and birth/merge/reorder cannot scramble a loop --
      it can only change that one loop's own field, never contaminate a neighbour.

HONEST WEAKNESS (documented, see notes): g ~ arc length only to first order. On a
STRONGLY non-convex loop (a re-entrant neck where r is multivalued in theta, or where
dr/dtheta is large so ds != r dtheta) the radius weighting under-corrects, leaving
residual wavelength fanning; and theta about the centroid is non-monotonic across a
deep concavity, which perturbs closure/registration there. This is the expected
control-baseline behaviour: it matches arc length on convex/round loops and degrades,
gracefully, on sharp/re-entrant ones.
"""
import math
import numpy as np

import validate_weave2 as J

LAMBDA = J.LAMBDA   # 4.0 mm  -- use the harness's own params, never re-define
AMP    = J.AMP      # 0.12 mm


def _theta_unwrapped(xy, c):
    """Angle of each point about centroid c, UNWRAPPED so it increases monotonically
    around a (convex) loop. Points come in polygon order from the harness resampler,
    so consecutive angle differences are small except for the single 2*pi wrap, which
    np.unwrap removes -> a continuous theta running over ~[theta_start, theta_start+2pi]."""
    th = np.arctan2(xy[:, 1] - c[1], xy[:, 0] - c[0])
    return np.unwrap(th)


def _field(loop, li, mode, state):
    xy = loop["xy"]
    L  = loop["L"]
    c  = loop["centroid"]
    n  = len(xy)
    if n < 3 or L <= 1e-9:
        return np.zeros(n)

    # angle + radius of every point about the centroid
    dx = xy[:, 0] - c[0]
    dy = xy[:, 1] - c[1]
    r  = np.hypot(dx, dy)
    th_raw = np.arctan2(dy, dx)

    # unwrapped, oriented angle so the swept coordinate is monotonic around the loop
    th = np.unwrap(th_raw)
    # ensure the loop is traversed in +theta order; if the resampler gave -theta
    # (clockwise) flip so dtheta >= 0 and the cumulative integral is increasing
    flipped = th[-1] < th[0]
    if flipped:
        th = th[::-1]
        r  = r[::-1]
        th_raw = th_raw[::-1]

    # ---- curvature-compensated angular coordinate: g = integral r dtheta -----------
    # Trapezoidal over the closed loop. dtheta between successive points; close the
    # wrap from the last point back to the first (its angle is th[0] + 2*pi).
    th_closed = np.concatenate([th, [th[0] + 2.0 * math.pi]])
    r_closed  = np.concatenate([r,  [r[0]]])
    dth   = np.diff(th_closed)                      # per-segment angular step (>=0)
    rmid  = 0.5 * (r_closed[:-1] + r_closed[1:])    # midpoint radius (r/r_mean folds
    seg   = rmid * dth                              #   into the 1/G normalisation)
    g     = np.concatenate([[0.0], np.cumsum(seg)]) # cumulative, length N+1
    G     = g[-1]                                   # total swept measure (closes exactly)
    g     = g[:-1]                                  # drop the closing duplicate -> length N
    if G <= 1e-9:
        return np.zeros(n)

    # ---- ADJACENT-LAYER REGISTRATION: re-anchor g=0 to a FIXED WORLD RAY -----------
    # The harness rolls the seam (xy[0]) RANDOMLY per layer, so g (which starts at the
    # seam) has a random origin -> phase would NOT register across layers. Re-anchor the
    # swept coordinate so g=0 sits where the centroid-ray points at a FIXED world
    # direction (+x, theta=0), independent of the seam. Because the centroid + outline
    # morph only GRADUALLY between layer N and N+1, the +x ray meets ~the same physical
    # (x,y) on both, so phase_N(x,y) ~ phase_{N+1}(x,y) -> controlled interlock.
    # Interpolate g AT theta=0 rather than snapping, for sub-sample stability.
    g0 = _g_at_world_ray(th_raw, g, G, target=0.0)
    g  = np.mod(g - g0, G)                          # g now runs 0..G from the +x ray

    # ---- closure: integer number of waves around the loop --------------------------
    # N from the TRUE perimeter (so wavelength ~ LAMBDA), forced integer so the phase
    # completes whole cycles. Subtracting a constant offset (g0) preserves closure
    # because phi = 2*pi*N*g/G is periodic in g with period G.
    n_wall = max(1, int(round(L / LAMBDA)))

    # phase: 2*pi*N * (swept fraction). g/G runs 0..1 around the loop, closing at 1.
    phi = 2.0 * math.pi * n_wall * (g / G)
    z   = AMP * J._sign(li, mode) * np.sin(phi)

    if flipped:                                     # restore original point order
        z = z[::-1]
    return z


def _g_at_world_ray(th_raw, g, G, target=0.0):
    """Swept coordinate g interpolated at the point where the centroid angle crosses a
    fixed world direction `target` (radians). Robust to the random seam: finds the
    crossing of (th_raw - target) wrapped to (-pi, pi], picks the crossing nearest the
    +x ray, and linearly interpolates g there. Falls back to the nearest sample."""
    d = np.mod(th_raw - target + math.pi, 2.0 * math.pi) - math.pi  # in (-pi, pi]
    n = len(d)
    # look for a sign change between consecutive samples that is NOT the 2*pi wrap
    best_i = None
    best_abs = None
    for i in range(n):
        a = d[i]
        b = d[(i + 1) % n]
        if a == 0.0:
            return float(g[i])
        if (a < 0.0) != (b < 0.0) and abs(a - b) < math.pi:  # genuine crossing
            t = a / (a - b)                                  # in [0,1]
            gi = g[i]
            gj = g[(i + 1) % n]
            # handle the cumulative wrap if the crossing straddles the g seam
            if (i + 1) % n == 0:
                gj = G
            gx = gi + (gj - gi) * t
            score = abs(a) * (1.0 - t) + abs(b) * t
            if best_abs is None or score < best_abs:
                best_abs = score
                best_i = float(np.mod(gx, G))
    if best_i is not None:
        return best_i
    # no clean crossing (deep concavity makes theta non-monotonic) -> nearest sample
    return float(g[int(np.argmin(np.abs(d)))])


# Stateless: each loop derives everything from its own geometry, so prepare() is a
# no-op. This is what makes islands independent and topology changes harmless.
J.register_method("curvature-comp-angle", dict(field=_field))


# ---------------------------------------------------------------- run via the judge
def _fmt(v, w=8, p=2):
    if isinstance(v, bool):
        return f"{'yes' if v else 'NO':>{w}s}"
    if v != v:
        return f"{'nan':>{w}s}"
    return f"{v:{w}.{p}f}"


def main():
    name = "curvature-comp-angle"

    # Build the SAME parts the harness uses, and the shared engine-event layers, exactly
    # as score_method() expects (it reads J._EVENTS[pname]). We do NOT touch harness code.
    parts_cache = {}
    for pname, (fac, is_mi) in J.PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)
    twist_layers = J.TWIST_PART[1]()
    J._EVENTS["twist"] = J.prop_event_counts(twist_layers)

    res = J.score_method(name, parts_cache)
    tw  = J.score_twist(name, twist_layers)

    parts = list(J.PARTS.keys())
    print(f"=== METHOD: {name} ===")
    print(f"params: lambda={LAMBDA}mm amp={AMP}mm\n")
    print(f"{'part':13s} {'interlk':>8s} {'il@evt':>8s} {'lam_cov':>8s} {'lamclmb':>8s} "
          f"{'amp_cov':>8s} {'closure':>8s} {'island':>8s}")
    print("-" * 78)
    for p in parts:
        r = res[p]
        print(f"{p:13s} {_fmt(r['interlock'],8,1)} {_fmt(r['il_event'],8,1)} "
              f"{_fmt(r['lam'],8,3)} {_fmt(r['lam_climb'],8,3)} {_fmt(r['amp'],8,3)} "
              f"{_fmt(r['clo'],8,3)} {_fmt(r['isl'],8)}")
    g = res["_global"]
    print(f"\nglobal: coverage={g['coverage']:.3f}  modulation={g['modulation']:.3f}")
    print(f"twist (scored separately): interlock_rms={_fmt(tw,0,1).strip()}%")

    # Machine-readable dump for the wrapper to parse if needed.
    print("\nSCORES_JSON_BEGIN")
    import json
    dump = {p: {k: (None if (isinstance(v, float) and v != v) else v)
                for k, v in res[p].items()} for p in parts}
    dump["_global"] = res["_global"]
    dump["twist"] = (None if (tw != tw) else tw)
    print(json.dumps(dump))
    print("SCORES_JSON_END")
    return res, tw


if __name__ == "__main__":
    main()
