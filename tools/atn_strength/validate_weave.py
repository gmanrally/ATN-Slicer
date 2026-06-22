#!/usr/bin/env python3
"""Validate woven-wall REGISTRATION on complex geometry, before any engine change.

Failure: the Z wave was anchored to a per-loop intrinsic coordinate (arc length
around the loop / angle from the loop centroid). That only registers across layers
if every layer's loop is the same shape. On a part whose cross-section changes with
height (airbox), the same physical (x,y) lands at a different arc-fraction each
layer and round(L/lambda) jumps -> layers fall out of phase -> moire.

Fix: a WORLD-SPACE phase field   Zoff(x,y,layer) = amp * sign(layer) * F(x,y).
Keyed on absolute position, so the same (x,y) gets the same modulation on EVERY
layer -> automatic vertical registration on any geometry. Two candidate fields:
  sum : F = (sin(kx)+sin(ky))/2          (square lattice)
  hex : F = sum of 3 plane waves 120 deg apart (more isotropic, higher coverage)

This compares the buggy arc method against both world fields, on synthetic complex
parts, with a co-location metric + per-layer heatmaps. PASS = world registration
error tiny AND a real weave (good modulation) AND arc clearly fails.

    python validate_weave.py
"""
import math
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LAMBDA, AMP, LAYER_H, NLAYERS, RES = 4.0, 0.12, 0.3, 24, 0.4


# ----------------------------------------------------------------- synthetic parts
def _resample_closed(poly, step=RES):
    pts = np.asarray(poly, float)
    seg = np.diff(np.vstack([pts, pts[:1]]), axis=0)
    seglen = np.hypot(seg[:, 0], seg[:, 1])
    L = seglen.sum()
    n = max(8, int(round(L / step)))
    cum = np.concatenate([[0], np.cumsum(seglen)])
    s = np.linspace(0, L, n, endpoint=False)
    out = []
    for si in s:
        j = min(np.searchsorted(cum, si, "right") - 1, len(pts) - 1)
        t = (si - cum[j]) / max(seglen[j], 1e-9)
        out.append(pts[j] + (pts[(j + 1) % len(pts)] - pts[j]) * t)
    return np.asarray(out), s, L


def part_morph_ellipse():
    layers = []
    for k in range(NLAYERS):
        a, b = 15 + 6 * math.sin(k * 0.45), 15 - 6 * math.sin(k * 0.45)
        th = np.linspace(0, 2 * math.pi, 220, endpoint=False) + math.radians(11 * k)
        layers.append(_resample_closed(np.column_stack([a * np.cos(th), b * np.sin(th)])))
    return layers


def part_elongated():
    """3:1 elongated ellipse (airbox-like). Exposes the angle field's fanning: radius
    swings 3x around the loop, so sin(N*theta) gives 3x wavelength variation."""
    layers = []
    for k in range(NLAYERS):
        a, b = 30.0 + 3.0 * math.sin(k * 0.3), 10.0 + 2.0 * math.sin(k * 0.3)
        th = np.linspace(0, 2 * math.pi, 320, endpoint=False) + math.radians(9 * k)
        layers.append(_resample_closed(np.column_stack([a * np.cos(th), b * np.sin(th)])))
    return layers


def part_dome():
    """Hemisphere: radius collapses with height. The single fixed-reference contour field
    fans badly here; phase propagation should follow the shrinking section cleanly."""
    layers = []
    for k in range(NLAYERS):
        z = k / NLAYERS
        r = 22.0 * math.sqrt(max(0.05, 1.0 - 0.95 * z * z))
        th = np.linspace(0, 2 * math.pi, max(24, int(2 * math.pi * r / RES)), endpoint=False)
        layers.append(_resample_closed(np.column_stack([r * np.cos(th), r * np.sin(th)])))
    return layers


def part_concave_gear():
    layers = []
    for k in range(NLAYERS):
        th = np.linspace(0, 2 * math.pi, 360, endpoint=False)
        r = 14 + 3.5 * np.sin(9 * th)
        th2 = (th + math.radians(7 * k))
        layers.append(_resample_closed(np.column_stack([r * np.cos(th2), r * np.sin(th2)])))
    return layers


# ----------------------------------------------------------------- fields
_K = 2 * math.pi / LAMBDA
_HEX_DIRS = [(math.cos(a), math.sin(a)) for a in (0.0, 2 * math.pi / 3, 4 * math.pi / 3)]
# normalise each field so max|F|~1 over a dense sample (so AMP means the same thing)
_g = np.linspace(0, LAMBDA, 64)
_GX, _GY = np.meshgrid(_g, _g)


def _F_sum(x, y):
    return 0.5 * (np.sin(_K * x) + np.sin(_K * y))


def _F_hex(x, y):
    return sum(np.sin(_K * (d[0] * x + d[1] * y)) for d in _HEX_DIRS) / 3.0


_NORM = {"sum": np.abs(_F_sum(_GX, _GY)).max(),
         "hex": np.abs(_F_hex(_GX, _GY)).max()}
_FIELD = {"sum": _F_sum, "hex": _F_hex}


def _sign(li, mode):
    return 1.0 if mode == "corrugated" else (1.0 if li % 2 == 0 else -1.0)


def zoff_world(xy, li, mode, field="hex"):
    F = _FIELD[field](xy[:, 0], xy[:, 1]) / _NORM[field]
    return AMP * _sign(li, mode) * F


_NCYC = 24          # whole cycles per revolution (set per part from a reference loop)
_CENTER = (0.0, 0.0)  # object centre (fixed for ALL layers -> registers)


def zoff_angle(xy, li, mode):
    """Angle-about-the-object-centre field: phase = N*theta. Single frequency around
    an encircling perimeter (NO beat), and theta is position-keyed so it registers
    across layers just like the world field."""
    th = np.arctan2(xy[:, 1] - _CENTER[1], xy[:, 0] - _CENTER[0])
    return AMP * _sign(li, mode) * np.sin(_NCYC * th)


_REF = None  # (ref_xy, ref_s, ref_L, N): a FIXED reference contour for the contour field


def make_reference(layers):
    """Densely-resampled mid-stack outline + cumulative arc-length + whole cycle count.
    Fixed for the object, so projecting onto it gives a position-keyed arc coordinate."""
    rxy, rs, rL = _resample_closed(layers[len(layers) // 2][0], step=RES / 2)
    return rxy, rs, rL, max(1, round(rL / LAMBDA))


def zoff_contour(xy, li, mode):
    """Phase = 2*pi*N*sR/refL, sR = arc-length of the nearest point on the FIXED reference
    contour. Uniform wavelength (follows the real shape, not a circle), uniform amplitude,
    and position-keyed so it registers across layers."""
    rxy, rs, rL, N = _REF
    j = np.argmin((xy[:, 0][:, None] - rxy[:, 0][None, :]) ** 2 +
                  (xy[:, 1][:, None] - rxy[:, 1][None, :]) ** 2, axis=1)
    return AMP * _sign(li, mode) * np.sin(2 * math.pi * N * rs[j] / rL)


_PROP = None  # dict li -> base phase array (radians), precomputed bottom-up


def _nearest_on_polyline(q, P):
    """Nearest point on a closed polyline P (Mx2) for each query q (Nx2): returns the
    segment index and the along-segment fraction t, so phase can be INTERPOLATED (not
    snapped to a vertex). This is what kills the per-layer noise."""
    A, B = P, np.roll(P, -1, axis=0)
    AB = B - A
    AB2 = (AB ** 2).sum(1) + 1e-12
    t = ((q[:, 0][:, None] - A[:, 0]) * AB[:, 0] + (q[:, 1][:, None] - A[:, 1]) * AB[:, 1]) / AB2
    t = np.clip(t, 0.0, 1.0)
    cx = A[:, 0] + t * AB[:, 0]
    cy = A[:, 1] + t * AB[:, 1]
    d2 = (q[:, 0][:, None] - cx) ** 2 + (q[:, 1][:, None] - cy) ** 2
    j = np.argmin(d2, axis=1)
    return j, t[np.arange(len(q)), j]


def build_propagation(layers):
    """Phase propagation done right: seed the bottom layer with arc-length/lambda (uniform
    wavelength); every higher layer INHERITS each point's phase from the nearest point on
    the layer below, INTERPOLATED along the nearest segment. Carried as a (cos,sin) vector
    so the seam wrap is harmless. The pattern is exact where the wall is vertical and flows
    smoothly with the surface where it curves -> registered everywhere, no fanning."""
    out, prev_xy, prev_cs = {}, None, None
    for li, (xy, s, L) in enumerate(layers):
        if prev_xy is None:
            ph = 2 * math.pi * max(1, round(L / LAMBDA)) * s / L
            cs = np.column_stack([np.cos(ph), np.sin(ph)])
        else:
            j, t = _nearest_on_polyline(xy, prev_xy)
            cs = prev_cs[j] * (1 - t)[:, None] + prev_cs[(j + 1) % len(prev_cs)] * t[:, None]
            cs /= np.maximum(np.hypot(cs[:, 0], cs[:, 1]), 1e-9)[:, None]
        out[li], prev_xy, prev_cs = cs, xy, cs
    return out


def zoff_prop(xy, li, mode):
    return AMP * _sign(li, mode) * _PROP[li][:, 1]  # sin component


def zoff_arc(xy, s, L, li, mode):
    cx, cy = xy[:, 0].mean(), xy[:, 1].mean()
    s0 = s[int(np.argmin(np.abs(np.arctan2(xy[:, 1] - cy, xy[:, 0] - cx))))]
    lam = L / max(1, round(L / LAMBDA))
    return AMP * _sign(li, mode) * np.sin(2 * math.pi * np.mod(s - s0, L) / lam)


def _zoff(layers, li, mode, method, field):
    xy, s, L = layers[li]
    if method == "arc":
        return zoff_arc(xy, s, L, li, mode)
    if method == "angle":
        return zoff_angle(xy, li, mode)
    if method == "contour":
        return zoff_contour(xy, li, mode)
    if method == "prop":
        return zoff_prop(xy, li, mode)
    return zoff_world(xy, li, mode, field)


def beat_cov(layers, method, field="sum"):
    """Along-wall AMPLITUDE uniformity on a mid-stack loop: CoV of the sliding-window
    peak-to-peak of Z. ~0 = uniform amplitude; high = beating (wavy<->straight bands)."""
    z = _zoff(layers, len(layers) // 2, "corrugated", method, field)
    W = max(6, int(round(1.5 * LAMBDA / RES)))
    p2p = np.array([np.ptp(np.take(z, range(i, i + W), mode="wrap")) for i in range(len(z))])
    return float(np.std(p2p) / (np.mean(p2p) + 1e-9))


def wav_cov(layers, method, field="sum"):
    """Along-wall WAVELENGTH uniformity on a mid-stack loop: CoV of the spacing between
    successive peaks/troughs of Z. ~0 = constant wavelength; high = fanning (angle field)."""
    z = _zoff(layers, len(layers) // 2, "corrugated", method, field)
    ext = np.where(np.diff(np.sign(np.diff(z))) != 0)[0]  # local extrema indices
    if len(ext) < 4:
        return 0.0
    spacing = np.diff(ext).astype(float)
    return float(np.std(spacing) / (np.mean(spacing) + 1e-9))


# ----------------------------------------------------------------- metric
def evaluate(layers, mode, method, field="hex", eps=0.3):
    """Co-location: between consecutive SAME-SIGN layers, match each point to the
    nearest point on the other layer (within eps mm) and record the Z mismatch.
    Registered -> mismatch ~ 0. Also report modulation = mean|Z|/amp (is it a real
    weave) and coverage = fraction with |Z|>0.3*amp."""
    step = 1 if mode == "corrugated" else 2
    Z = [_zoff(layers, li, mode, method, field) for li in range(len(layers))]
    mism, modz, cov, ntot = [], 0.0, 0, 0
    for z in Z:
        modz += np.abs(z).sum(); cov += int(np.count_nonzero(np.abs(z) > 0.3 * AMP)); ntot += len(z)
    for li in range(len(layers) - step):
        xy0, xy1 = layers[li][0], layers[li + step][0]
        z0, z1 = Z[li], Z[li + step]
        for i in range(len(xy0)):
            d = np.hypot(xy1[:, 0] - xy0[i, 0], xy1[:, 1] - xy0[i, 1])
            j = int(np.argmin(d))
            if d[j] <= eps:
                mism.append(z0[i] - z1[j])
    rms = float(np.sqrt(np.mean(np.square(mism)))) if mism else float("nan")
    return rms, modz / max(ntot, 1) / AMP, cov / max(ntot, 1)


# ----------------------------------------------------------------- viz
def render(layers, name):
    """Top: Z along one wall (the beat) for sum vs angle. Bottom: per-layer heatmaps
    (both register). Angle = uniform amplitude AND registered = the goal."""
    fig = plt.figure(figsize=(13, 8))
    li = len(layers) // 2
    methods = [("contour — fans on dome", "contour", None),
               ("phase propagation — uniform", "prop", None)]
    for c, (title, method, field) in enumerate(methods):
        ax = fig.add_subplot(2, 2, c + 1)
        ax.plot(_zoff(layers, li, "corrugated", method, field), lw=0.6)
        ax.set_title(f"{name}: {title}", fontsize=9)
        ax.set_xlabel("point along wall"); ax.set_ylabel("Z (mm)")
        ax.set_ylim(-AMP * 1.25, AMP * 1.25)
        axh = fig.add_subplot(2, 2, c + 3, aspect="equal")
        for lj in (0, 2, 4, 6):
            xy = layers[lj][0]
            sc = axh.scatter(xy[:, 0], xy[:, 1], c=_zoff(layers, lj, "nested", method, field) * _sign(lj, "nested"),
                             cmap="coolwarm", vmin=-AMP, vmax=AMP, s=5)
        axh.set_title("layers 0/2/4/6 overlaid (registered = consistent colour at a spot)", fontsize=8)
        axh.set_xticks([]); axh.set_yticks([])
    fig.colorbar(sc, ax=fig.axes, shrink=0.5, label="Z (mm)")
    fig.suptitle("sum field registers but beats (left); angle-about-centre registers AND is uniform (right)", fontsize=10)
    fig.savefig("validate_weave.png", dpi=130, bbox_inches="tight")


def validate_brick():
    """Brick stagger must key on a STABLE shell index (inset_idx, 0=outer), not the
    per-layer loop iteration counter. Model layers whose island count/order varies
    (the complex-geometry stressor) and check whether each physical shell keeps the
    same brick course across layers."""
    # Each layer: walls in ITERATION order as (island, inset_idx). The brick course a
    # shell gets MUST depend only on its shell index, so the same shell bricks the same
    # everywhere it appears (across islands and layers) and courses stack vertically.
    # Stressor: island count varies (a hole opens at layer 4) AND iteration order is not
    # guaranteed stable (islands emitted in different order on alternate layers).
    layers = []
    for k in range(10):
        islands = 1 if k < 4 else 2
        order = list(range(islands))
        if k % 2:                      # the slicer does NOT guarantee island order
            order = order[::-1]
        layers.append([(isl, inset) for isl in order for inset in range(3)])

    print("brick keying — does a given SHELL INDEX always get the same brick course?")
    for key in ("loop", "inset"):
        per_inset = {}                 # inset_idx -> set of parities ever assigned to it
        for walls in layers:
            for loop_idx, (isl, inset) in enumerate(walls):
                parity = (loop_idx & 1) if key == "loop" else (inset & 1)
                per_inset.setdefault(inset, set()).add(parity)
        bad = sum(1 for v in per_inset.values() if len(v) > 1)
        detail = ", ".join(f"inset{i}->{sorted(v)}" for i, v in sorted(per_inset.items()))
        print(f"  key={key:5s}  ambiguous shell indices: {bad}/{len(per_inset)}   [{detail}]"
              f"   {'UNSTABLE' if bad else 'stable'}")


def main():
    global _NCYC, _REF, _PROP
    parts = [("elongated 3:1", part_elongated),
             ("dome (collapsing)", part_dome),
             ("concave gear", part_concave_gear)]
    print(f"params: lambda={LAMBDA}mm amp={AMP}mm layers={NLAYERS}   (reg %amp; beat/wav: 0=good)\n")
    print(f"{'part':18s} {'method':9s} {'reg':>5s} {'beat':>6s} {'wav':>6s}")
    print("-" * 50)
    res = {}
    for pname, pfn in parts:
        layers = pfn()
        _NCYC = max(1, round(layers[0][2] / LAMBDA))
        _REF = make_reference(layers)
        _PROP = build_propagation(layers)
        for method in ("angle", "contour", "prop"):
            rms = evaluate(layers, "nested", method, "sum")[0]
            res[(pname, method)] = (rms, beat_cov(layers, method), wav_cov(layers, method))
            v = res[(pname, method)]
            print(f"{pname:18s} {method:9s} {100*v[0]/AMP:4.0f}% {v[1]:6.2f} {v[2]:6.2f}")
        print()
    dome = part_dome()
    _NCYC = max(1, round(dome[0][2] / LAMBDA)); _REF = make_reference(dome); _PROP = build_propagation(dome)
    render(dome, "dome (collapsing section)")
    print("wrote validate_weave.png\n")

    sel = lambda t, i: [v[i] for (p, tt), v in res.items() if tt == t]
    prop_wav = all(w < 0.25 for w in sel("prop", 2))
    prop_beat = all(b < 0.12 for b in sel("prop", 1))
    cd, pd = res[("dome (collapsing)", "contour")], res[("dome (collapsing)", "prop")]
    contour_fans = cd[2] > 0.30
    print(f"propagation wavelength-uniform everywhere (<0.25): {prop_wav}   {[round(w,2) for w in sel('prop',2)]}")
    print(f"propagation amplitude-uniform (<0.12): {prop_beat}   {[round(b,2) for b in sel('prop',1)]}")
    print(f"contour FANS on the dome where propagation does NOT: {contour_fans}   (dome wav: contour {cd[2]:.2f} vs prop {pd[2]:.2f})")
    print("\nRESULT:", "PASS — phase propagation stays uniform where the contour field fans (dome)"
          if (prop_wav and prop_beat and contour_fans) else "REVIEW")


if __name__ == "__main__":
    main()
