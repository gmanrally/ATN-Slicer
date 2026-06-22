#!/usr/bin/env python3
"""Candidate method: stateless-anchored-arclen.

STATELESS per-island anchored arc-length phase field.

Per loop, INDEPENDENTLY (no cross-layer phase recurrence):
  * anchor s=0 at the point of MAX projection onto a FIXED world direction
    (the object axis). This is a purely geometric anchor that moves continuously
    with the wall, so the same physical (x,y) anchors to nearly the same place on
    adjacent (nearly identical) layers -> ADJACENT-LAYER registration for free.
  * N = round(L / lambda_target)  -> exact INTEGER number of waves per closed loop
    (closure is structurally guaranteed: phase advances by 2*pi*N over the full L).
  * phase(point) = 2*pi*N*(s - s_anchor)/L, wrapped on L.
  * nested mode: a half-wave (pi) stagger by layer parity so layer N's peak sits
    over layer N+1's trough. corrugated: same phase every layer.

The ONLY cross-layer state is an N-HYSTERESIS: a per-loop integer wave count that
resists flipping when round(L/lambda) sits on a boundary, matched to the loop one
layer below by geometry (centroid + area), NOT by island_id. Phase itself stays
PURELY geometric -- the hysteresis only stabilises the integer N so the wavelength
doesn't jump by a whole wave between adjacent layers. This keeps registration
controlled without importing a phase recurrence (which is what fans / mis-binds).

WHY THIS REGISTERS (req 1, headline):
  Two adjacent layers are nearly the same polygon. The anchor (argmax projection
  onto a fixed direction) is a continuous functional of the polygon, so it lands at
  ~the same arc position. s/L is the normalised arc coordinate; on near-identical
  loops the same (x,y) has ~the same s/L. N is stabilised by hysteresis. Hence
  phase(N) ~= phase(N+1) at a shared physical point, and the nested pi flip makes
  them antiphase -> the sum cancels -> low interlock RMS. No global registration is
  attempted (irrelevant to strength); only adjacency is optimised, which a stateless
  intrinsic field does cleanly because it has no accumulating drift.

Honest weaknesses (reported):
  * The anchor is an argmax: where the projection has a flat/degenerate maximum (a
    near-circular loop, or a wall that is locally parallel to the axis direction) the
    argmax can jump discretely between adjacent layers -> a localised registration
    glitch near that one anchor point. A blended/soft anchor would reduce this; we
    keep the hard argmax to stay genuinely stateless and report the residual.
  * s/L is a UNIFORM arc parameter, so wavelength is uniform in arc length but the
    physical wavelength varies if the loop's local sampling density varies; on these
    resampled loops that is negligible.
  * Hysteresis matching is geometric (centroid+area); at a true centroid near-collision
    it can briefly stabilise N from the wrong neighbour, but since it only sets the
    INTEGER N (not phase) the visible effect is at most a one-wave wavelength step,
    not a phase scramble.
"""
import math
import numpy as np

import validate_weave2 as J


LAMBDA = J.LAMBDA
AMP = J.AMP

# Fixed world direction for the anchor (the "object axis"). Any fixed unit vector
# works; a slightly irrational angle avoids axis-aligned degeneracy on the boxes /
# ellipses whose extremes sit exactly on x or y.
_ANCHOR_DIR = np.array([math.cos(0.3), math.sin(0.3)])


def _anchor_arc(xy, s, L):
    """SUB-SAMPLE arc length of the point with MAX projection onto the fixed world
    direction. Returns a continuous s_anchor (mm).

    A plain argmax (vertex index) is quantised: between two near-identical adjacent
    layers the discrete maximum can hop by a few samples even though the true extreme
    moved a fraction of a sample -> a phase offset that is a multiple of the one-sample
    step (2*pi*N*RES/L). That quantisation, not the geometry, was the dominant
    adjacent-layer registration error. We remove it by fitting a parabola through the
    projection at (argmax-1, argmax, argmax+1) and taking the vertex, then interpolating
    the arc length there. This makes s_anchor a CONTINUOUS functional of the polygon, so
    a fractional shift of the true extreme gives a fractional (matching) phase shift on
    both layers -> they still cancel under the nested flip.
    """
    proj = xy @ _ANCHOR_DIR
    i = int(np.argmax(proj))
    n = len(xy)
    ip, inx = (i - 1) % n, (i + 1) % n
    y0, y1, y2 = proj[ip], proj[i], proj[inx]
    denom = (y0 - 2.0 * y1 + y2)
    # parabola vertex offset in [-0.5, 0.5] samples (0 if flat/degenerate)
    delta = 0.0 if abs(denom) < 1e-12 else 0.5 * (y0 - y2) / denom
    delta = max(-0.5, min(0.5, delta))
    # interpolate arc length across the seam-safe neighbour in the offset direction.
    s_i = s[i]
    if delta >= 0.0:
        ds = (s[inx] - s_i) % L
        return (s_i + delta * ds) % L
    ds = (s_i - s[ip]) % L
    return (s_i + delta * ds) % L


def _match_below(centroid, area, prev_records):
    """Geometry-only nearest match to a loop one layer below: nearest centroid within
    that loop's own radius sqrt(area/pi), as a tie-break prefer closest area. Returns
    the matched record or None. Mirrors the engine's centroid gate but is used ONLY to
    inherit a stabilised integer N (hysteresis), never phase."""
    if not prev_records:
        return None
    best = math.sqrt(max(area, 1.0) / math.pi)
    match = None
    for rec in prev_records:
        dc = float(np.hypot(centroid[0] - rec["centroid"][0],
                            centroid[1] - rec["centroid"][1]))
        if dc < best:
            best = dc
            match = rec
    return match


def prepare(layers):
    """Bottom-up pass that computes, per loop, the stabilised integer wave count N via
    hysteresis against the matched loop below. NO phase is propagated -- only N.

    state[li] is a list of records {centroid, area, N} aligned with the engine's view
    (geometry only). field() resolves a loop to its record by nearest centroid, exactly
    like the shipped matcher, so a hysteresis mis-bind is what would ship."""
    states = []
    prev = None
    for loops in layers:
        cur = []
        for lp in loops:
            L = lp["L"]
            area = lp["area"]
            cen = lp["centroid"]
            n_raw = max(1, int(round(L / LAMBDA)))
            N = n_raw
            m = _match_below(cen, area, prev)
            if m is not None:
                # Hysteresis: keep the previous N unless round(L/lambda) has moved by
                # MORE than half a wave away from it (a real, not boundary, change).
                # |L/lambda - N_prev| must exceed 0.5 to switch; then snap to n_raw.
                if abs(L / LAMBDA - m["N"]) <= 0.5 and m["N"] >= 1:
                    N = m["N"]
                else:
                    N = n_raw
            cur.append(dict(centroid=np.asarray(cen, float), area=float(area), N=int(N)))
        states.append(cur)
        prev = cur
    return states


def _lookup_N(loop, li, state):
    """Resolve this loop's stabilised N the engine way: nearest centroid among this
    layer's records (no island_id). Falls back to round(L/lambda) if state missing."""
    if state is None or li >= len(state) or not state[li]:
        return max(1, int(round(loop["L"] / LAMBDA)))
    cen = loop["centroid"]
    rec = min(state[li], key=lambda r: float(np.hypot(cen[0] - r["centroid"][0],
                                                      cen[1] - r["centroid"][1])))
    return rec["N"]


def field(loop, li, mode, state):
    """Z modulation (mm) for this loop's xy points.

    phase = 2*pi*N*(s - s_anchor)/L  (closed: advances 2*pi*N over L -> integer waves).
    nested half-wave stagger: add pi on odd layers so peak(N) sits over trough(N+1).
    """
    xy = loop["xy"]
    s = loop["s"]
    L = loop["L"]

    N = _lookup_N(loop, li, state)

    s_anchor = _anchor_arc(xy, s, L)

    # arc phase, wrapped on L so the anchor offset is seam-safe and closure is exact.
    ds = np.mod(s - s_anchor, L)
    phase = 2.0 * math.pi * N * ds / L

    # Nested antiphase: half-wave (pi) stagger by layer parity. This is the interlock
    # stagger -- equivalent to the harness _sign() flip but applied as a PHASE shift so
    # it composes cleanly with the arc phase. corrugated: no stagger.
    if mode != "corrugated":
        phase = phase + (math.pi if (li % 2) else 0.0)

    return AMP * np.sin(phase)


J.register_method("stateless_anchored_arclen",
                  dict(prepare=prepare, field=field))


def _run():
    """Build the parts + engine events exactly as the harness main() does (the harness
    does NOT expose a helper for this, so we replicate the public sequence -- without
    editing the harness), then score our method and print the real numbers."""
    parts_cache = {}
    for pname, (fac, is_mi) in J.PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)

    twist_layers = J.TWIST_PART[1]()
    J._EVENTS["twist"] = J.prop_event_counts(twist_layers)

    name = "stateless_anchored_arclen"
    res = J.score_method(name, parts_cache)
    tw = J.score_twist(name, twist_layers)

    # Also score the shipped 'prop' for a side-by-side reference on the headline metric.
    prop = J.score_method("prop", parts_cache)

    parts = list(J.PARTS.keys())
    print("=== stateless_anchored_arclen :: REAL SCORES ===")
    print(f"params: lambda={LAMBDA}mm amp={AMP}mm\n")

    print("ADJACENT-LAYER INTERLOCK RMS (% amp, 0=ideal, ~141=random):")
    hdr = f"{'part':14s} {'ours':>8s} {'prop':>8s}"
    print(hdr)
    print("-" * len(hdr))
    for p in parts:
        print(f"{p:14s} {res[p]['interlock']:8.2f} {prop[p]['interlock']:8.2f}")
    print(f"{'twist':14s} {tw:8.2f} {J.score_twist('prop', twist_layers):8.2f}")

    print("\nFULL METRICS (ours):")
    print(f"{'part':14s} {'intrlk':>7s} {'il@evt':>7s} {'lam':>6s} {'lamclm':>7s} "
          f"{'amp':>6s} {'clo':>6s} {'isl':>5s} {'rsd':>4s} {'msb':>4s}")
    for p in parts:
        r = res[p]
        isl = "yes" if r["isl"] else "NO"
        print(f"{p:14s} {r['interlock']:7.2f} {r['il_event']:7.2f} {r['lam']:6.3f} "
              f"{r['lam_climb']:7.3f} {r['amp']:6.3f} {r['clo']:6.3f} {isl:>5s} "
              f"{r['reseeds']:4d} {r['misbinds']:4d}")

    g = res["_global"]
    print(f"\n_global: coverage={g['coverage']:.3f} modulation={g['modulation']:.3f}")

    return res, tw, prop


if __name__ == "__main__":
    _run()
