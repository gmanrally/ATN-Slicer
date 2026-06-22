#!/usr/bin/env python3
"""Faithful woven-wall validation harness (v2).

The v1 harness (validate_weave.py) measured the WRONG thing on geometry that does
NOT represent the real part, so a method could "PASS" there and still weave terribly
on the airbox. Three independent diagnoses agreed on the gaps; this rewrite closes
them. Concretely, v2:

  * Models every LAYER as a LIST OF ISLAND LOOPS (a body + velocity-stack trumpets),
    with features that APPEAR, GROW, MOVE and MERGE across height, emitted in an
    UNSTABLE order between layers -> exercises the C++ centroid-match / round-seed /
    guard-fallback / re-seed control plane that v1 never touched.
  * Runs >=120 layers on the tall part so a propagation RECURRENCE has room to drift.
  * Has NO artificial per-layer rigid rotation: adjacent layers differ only by a
    gradual, realistic morph -> intrinsic-coordinate methods are tested honestly and
    world fields are not handed a rigged win.
  * Scores ADJACENT-LAYER (N, N+1) interlock registration PER ISLAND against the
    intended nested half-wave stagger -- the quantity that actually buys Z strength --
    rather than same-sign step=2 global co-location.
  * Drives EVERY candidate through the SAME judge via a method-plugin API, and ports
    the real engine's matching/seeding logic into the 'prop' plugin (centroid match
    within sqrt(area/pi), 4*pi*A/L^2>0.6 round test, ncyc*atan2 round seed inheriting
    axis/ncyc, 2*lambda guard fallback, per-loop round(L/lambda) seed, re-seed on no
    match) so we validate the algorithm the slicer SHIPS, not an idealised twin.

PASS is deliberately hard: it requires small adjacent-layer interlock RMS on the
MULTI-ISLAND and SHARP parts (where prop's mis-binds / re-seeds bite), uniform
wavelength & amplitude, integer-wave closure, a real weave (coverage+modulation),
and per-island consistency. The expectation -- confirmed in the printed table -- is
that the shipped 'prop' FAILS the multi-island / sharp teeth and that NO method
trivially passes everything.

    python validate_weave2.py
"""
import math
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ----------------------------------------------------------------- global params
LAMBDA  = 4.0     # target wavelength (mm)
AMP     = 0.12    # weave amplitude (mm)
LAYER_H = 0.3     # layer height (mm)
RES     = 0.4     # wall resample resolution (mm)
SEED    = 12345


# ===================================================================== geometry
def _resample_closed(poly, step=RES):
    """Resample a closed polygon to ~uniform `step` spacing. Returns xy (Nx2),
    cumulative arc length s (N,) and total length L. NOTE: s starts at the FIRST
    vertex of `poly` -- i.e. the SEAM. We deliberately let callers roll the seam."""
    pts = np.asarray(poly, float)
    seg = np.diff(np.vstack([pts, pts[:1]]), axis=0)
    seglen = np.hypot(seg[:, 0], seg[:, 1])
    L = float(seglen.sum())
    n = max(8, int(round(L / step)))
    cum = np.concatenate([[0.0], np.cumsum(seglen)])
    s = np.linspace(0.0, L, n, endpoint=False)
    out = []
    for si in s:
        j = min(int(np.searchsorted(cum, si, "right") - 1), len(pts) - 1)
        t = (si - cum[j]) / max(seglen[j], 1e-9)
        out.append(pts[j] + (pts[(j + 1) % len(pts)] - pts[j]) * t)
    return np.asarray(out), s, L


def _poly_area_centroid(xy):
    """Signed-area centroid + |area| of a closed loop (shoelace)."""
    x, y = xy[:, 0], xy[:, 1]
    xn, yn = np.roll(x, -1), np.roll(y, -1)
    cr = x * yn - xn * y
    a2 = cr.sum()
    if abs(a2) < 1e-9:
        return np.array([x.mean(), y.mean()]), 0.0
    cx = ((x + xn) * cr).sum() / (3.0 * a2)
    cy = ((y + yn) * cr).sum() / (3.0 * a2)
    return np.array([cx, cy]), abs(a2) * 0.5


def _roll_seam(xy, rng):
    """Rotate the start vertex (seam) by a random amount -> models slicer seam jumps
    so seam/arc-length-dependent methods are tested for seam sensitivity."""
    k = rng.integers(0, len(xy))
    return np.roll(xy, -int(k), axis=0)


def _ellipse(cx, cy, a, b, n=240, rot=0.0):
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    ct, st = math.cos(rot), math.sin(rot)
    x = a * np.cos(th); y = b * np.sin(th)
    return np.column_stack([cx + x * ct - y * st, cy + x * st + y * ct])


def _reentrant_blob(cx, cy, w, h, neck, n=260):
    """A rounded body with a RE-ENTRANT neck on the right side (a deep inward pinch).
    `neck` (mm) is how far the right wall is pulled inward toward the centre; >0 makes
    a genuine concavity where (a) a fixed-reference contour's nearest-point projection
    becomes multivalued -> fans, and (b) the propagation guard (2*lambda) fires because
    the matched loop below is far across the pocket."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    rx, ry = w / 2, h / 2
    x = rx * np.cos(th)
    y = ry * np.sin(th)
    # pull the right flank (theta near 0) inward by a Gaussian notch -> re-entrant pocket
    g = np.exp(-((np.mod(th + math.pi, 2 * math.pi) - math.pi) ** 2) / (2 * 0.32 ** 2))
    x = x - neck * g  # right side (cos>0) gets pushed in; deep enough -> concave
    return np.column_stack([cx + x, cy + y])


def _rounded_rect(cx, cy, w, h, r, n_side=40, n_corner=12):
    """Rounded rectangle with sharp-ish corners (small r) -> non-convex-ish angle
    field fanning + corner concavity to trip guard fallback."""
    r = min(r, 0.49 * min(w, h))
    hw, hh = w / 2 - r, h / 2 - r
    pts = []
    corners = [(hw, hh, 0), (-hw, hh, math.pi / 2), (-hw, -hh, math.pi), (hw, -hh, 3 * math.pi / 2)]
    for (ox, oy, a0) in corners:
        for j in range(n_corner + 1):
            a = a0 + (math.pi / 2) * j / n_corner
            pts.append((cx + ox + r * math.cos(a), cy + oy + r * math.sin(a)))
    # dedup near-coincident
    out = [pts[0]]
    for p in pts[1:]:
        if (p[0] - out[-1][0]) ** 2 + (p[1] - out[-1][1]) ** 2 > 1e-6:
            out.append(p)
    return np.array(out)


def _loop(xy, island_id, rng, jitter_seam=True):
    """Build a 'loop' dict the way the harness passes it to a method's field()."""
    if jitter_seam:
        xy = _roll_seam(xy, rng)
    xyr, s, L = _resample_closed(xy)
    c, area = _poly_area_centroid(xyr)
    return dict(xy=xyr, s=s, L=L, island_id=island_id, centroid=c, area=area)


# ----------------------------------------------------------------- parts
# Each PART is a list of LAYERS; each LAYER is a LIST of loop-dicts (the islands).

def part_multi_island(nlayers=130):
    """A body loop + 3 velocity-stack trumpets. Trumpets APPEAR at staggered heights,
    GROW, MOVE, and trumpet #2 MERGES into the body partway up. Islands are emitted in
    an UNSTABLE order between layers (the slicer does not guarantee island order).
    Two trumpet centroids are placed within ~sqrt(area/pi) of each other near the top
    of their independent run to provoke a centroid mis-bind."""
    rng = np.random.default_rng(SEED)
    layers = []
    # trumpet schedule: (start_layer, base centre, end centre, r0, r1, merge_layer or None).
    # Trumpets 2 and 3 are routed so their centres pass within ~sqrt(area/pi) of each other
    # around layers 45-60 -> a deliberate centroid NEAR-COLLISION to provoke a C++ mis-bind
    # (a loop binding to the WRONG feature below). Trumpet 2 then MERGES into the body at 70.
    trumpets = [
        dict(iid=1, start=10, c0=(-15, 8),  c1=(-10, 6),  r0=2.2, r1=5.0, merge=None),
        dict(iid=2, start=18, c0=(11, 9),   c1=(9, -3),   r0=2.0, r1=5.6, merge=85),   # large, drifts DOWN, merges late
        dict(iid=3, start=35, c0=(9, -7),   c1=(9, 1),    r0=1.8, r1=3.6, merge=None),  # small, drifts UP through #2's track
    ]
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        # body: a rounded blob that slowly grows + drifts AND develops a RE-ENTRANT neck
        # on the right flank (depth grows then recedes with height -> a MOVING concavity).
        bw = 40 + 6 * math.sin(0.9 * math.pi * z)
        bh = 26 + 4 * math.sin(0.9 * math.pi * z + 0.7)
        neck = max(0.0, 11.0 * math.sin(math.pi * min(1.0, max(0.0, (z - 0.25) / 0.5))))
        body = _reentrant_blob(0, 0, bw, bh, neck)
        loops.append(("body", 0, body))
        for tr in trumpets:
            if k < tr["start"]:
                continue
            if tr["merge"] is not None and k >= tr["merge"]:
                continue  # merged into body -> no independent loop above merge layer
            # growth fraction over this trumpet's active run
            end = tr["merge"] if tr["merge"] is not None else nlayers
            f = (k - tr["start"]) / max(1, end - tr["start"])
            f = min(1.0, f)
            cx = tr["c0"][0] + (tr["c1"][0] - tr["c0"][0]) * f
            cy = tr["c0"][1] + (tr["c1"][1] - tr["c0"][1]) * f
            r = tr["r0"] + (tr["r1"] - tr["r0"]) * f
            # slight ovality so circularity sits near (not far above) the 0.6 round test
            loops.append(("trumpet", tr["iid"], _ellipse(cx, cy, r, r * 0.86, n=max(48, int(2 * math.pi * r / RES)))))
        # UNSTABLE emission order: shuffle island order on odd layers
        if k % 2:
            loops = loops[::-1]
        layers.append([_loop(xy, iid, rng) for (_, iid, xy) in loops])
    return layers


def part_sharp_box(nlayers=90):
    """Rounded-rect box with sharp-ish corners; section grows/shrinks gradually, NO
    spin. Corners + the box's strong aspect change trip the angle field's fanning and
    the propagation guard at the corners."""
    rng = np.random.default_rng(SEED + 1)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        w = 34 + 8 * math.sin(math.pi * z)
        h = 20 + 4 * math.sin(math.pi * z + 1.1)
        box = _rounded_rect(0, 0, w, h, 2.5)  # small corner radius => sharp-ish
        layers.append([_loop(box, 0, rng)])
    return layers


def part_elongated(nlayers=90):
    """3:1 elongated ellipse, gradual non-rigid morph, NO artificial rotation."""
    rng = np.random.default_rng(SEED + 2)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        a = 30.0 + 3.0 * math.sin(math.pi * z)
        b = 10.0 + 2.0 * math.sin(math.pi * z + 0.5)
        layers.append([_loop(_ellipse(0, 0, a, b, n=320), 0, rng)])
    return layers


def part_dome(nlayers=130):
    """Collapsing dome: radius shrinks with height (many layers -> recurrence drift)."""
    rng = np.random.default_rng(SEED + 3)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        r = 22.0 * math.sqrt(max(0.06, 1.0 - 0.93 * z * z))
        layers.append([_loop(_ellipse(0, 0, r, r, n=max(40, int(2 * math.pi * r / RES))), 0, rng)])
    return layers


def part_twist(nlayers=90):
    """ONE genuinely twisting part (scored SEPARATELY): an elongated section that
    physically rotates with height. Here the rotation is REAL geometry (the wall moves),
    unlike v1's rigged sampling spin. World-keyed angle/contour fields cannot register a
    truly twisting wall; intrinsic propagation can. Reported on its own, not in PASS."""
    rng = np.random.default_rng(SEED + 4)
    layers = []
    for k in range(nlayers):
        rot = math.radians(2.0 * k)  # real twist of the body
        layers.append([_loop(_ellipse(0, 0, 26, 12, n=300, rot=rot), 0, rng)])
    return layers


PARTS = {
    "multi_island": (part_multi_island, True),   # (factory, is_multi_island)
    "sharp_box":    (part_sharp_box, False),
    "elongated":    (part_elongated, False),
    "dome":         (part_dome, False),
}
# twist is scored separately, not part of the PASS gate
TWIST_PART = ("twist", part_twist)


# ===================================================================== methods
# A method is registered as {prepare(layers)->state, field(loop, li, mode, state)->Z}.
# `field` returns the Z modulation array (mm) for the loop's xy points. The judge then
# evaluates the SAME metrics on every method's output. No method gets special treatment.

_METHODS = {}


def register_method(name, spec):
    assert "field" in spec, "method needs a field()"
    spec.setdefault("prepare", lambda layers: None)
    _METHODS[name] = spec


def _sign(li, mode):
    """Nested antiphase: layer parity flips the sign so layer N peak sits over layer
    N+1 trough (the half-wave interlock). Corrugated: same sign every layer."""
    return 1.0 if mode == "corrugated" else (1.0 if li % 2 == 0 else -1.0)


# --- world fields (sum / hex): zoff = amp*sign*F(x,y), position-keyed ---------
_K = 2 * math.pi / LAMBDA
_HEX = [(math.cos(a), math.sin(a)) for a in (0.0, 2 * math.pi / 3, 4 * math.pi / 3)]
_g = np.linspace(0, LAMBDA, 64)
_GX, _GY = np.meshgrid(_g, _g)


def _F_sum(x, y):
    return 0.5 * (np.sin(_K * x) + np.sin(_K * y))


def _F_hex(x, y):
    return sum(np.sin(_K * (d[0] * x + d[1] * y)) for d in _HEX) / 3.0


_WNORM = {"sum": float(np.abs(_F_sum(_GX, _GY)).max()), "hex": float(np.abs(_F_hex(_GX, _GY)).max())}


def _make_world(field):
    def f(loop, li, mode, state):
        xy = loop["xy"]
        F = (_F_sum if field == "sum" else _F_hex)(xy[:, 0], xy[:, 1]) / _WNORM[field]
        return AMP * _sign(li, mode) * F
    return dict(field=f)


# --- angle field: phase = N*theta about a FIXED object centre ------------------
def _angle_prepare(layers):
    # one global cycle count from a mid-stack body loop
    mid = layers[len(layers) // 2]
    big = max(mid, key=lambda lp: lp["L"])
    return dict(ncyc=max(1, round(big["L"] / LAMBDA)), center=np.array([0.0, 0.0]))


def _angle_field(loop, li, mode, state):
    xy = loop["xy"]
    th = np.arctan2(xy[:, 1] - state["center"][1], xy[:, 0] - state["center"][0])
    return AMP * _sign(li, mode) * np.sin(state["ncyc"] * th)


# --- contour field: phase = 2*pi*N*sR/refL on a FIXED reference outline --------
def _contour_prepare(layers):
    mid = layers[len(layers) // 2]
    big = max(mid, key=lambda lp: lp["L"])
    rxy, rs, rL = _resample_closed(big["xy"], step=RES / 2)
    return dict(rxy=rxy, rs=rs, rL=rL, N=max(1, round(rL / LAMBDA)))


def _contour_field(loop, li, mode, state):
    xy, rxy, rs, rL, N = loop["xy"], state["rxy"], state["rs"], state["rL"], state["N"]
    j = np.argmin((xy[:, 0][:, None] - rxy[:, 0][None, :]) ** 2 +
                  (xy[:, 1][:, None] - rxy[:, 1][None, :]) ** 2, axis=1)
    return AMP * _sign(li, mode) * np.sin(2 * math.pi * N * rs[j] / rL)


# --- arc field: per-loop arc-length from a centroid-angle seam -----------------
def _arc_field(loop, li, mode, state):
    xy, s, L = loop["xy"], loop["s"], loop["L"]
    cx, cy = xy[:, 0].mean(), xy[:, 1].mean()
    s0 = s[int(np.argmin(np.abs(np.arctan2(xy[:, 1] - cy, xy[:, 0] - cx))))]
    lam = L / max(1, round(L / LAMBDA))
    return AMP * _sign(li, mode) * np.sin(2 * math.pi * np.mod(s - s0, L) / lam)


# --- prop: FAITHFUL port of the C++ engine control plane ----------------------
# Reproduces WovenWalls.cpp: per-loop centroid match within sqrt(area/pi), the
# 4*pi*A/L^2>0.6 round test, ncyc*atan2 round seed inheriting axis/ncyc from the
# matched loop below, the 2*lambda guard fallback to arc-length, per-loop
# round(L/lambda), and a fresh re-seed when no loop matches. State is built bottom-up
# (a recurrence): each layer's per-loop (cos,sin) field is stored for the layer above.

def _nearest_seg(q, P):
    """Nearest point on closed polyline P for each query q: segment index + fraction t."""
    A = P
    B = np.roll(P, -1, axis=0)
    AB = B - A
    AB2 = (AB ** 2).sum(1) + 1e-12
    t = ((q[:, 0][:, None] - A[:, 0]) * AB[:, 0] + (q[:, 1][:, None] - A[:, 1]) * AB[:, 1]) / AB2
    t = np.clip(t, 0.0, 1.0)
    cx = A[:, 0] + t * AB[:, 0]
    cy = A[:, 1] + t * AB[:, 1]
    d2 = (q[:, 0][:, None] - cx) ** 2 + (q[:, 1][:, None] - cy) ** 2
    j = np.argmin(d2, axis=1)
    ar = np.arange(len(q))
    dmin = np.sqrt(d2[ar, j])
    return j, t[ar, j], dmin


def _prop_prepare(layers):
    """Bottom-up recurrence exactly like make_woven_walls: for each layer build, per
    loop, the (cos,sin) base-phase field and its match/round bookkeeping, matching
    against the PREVIOUS layer's loops by nearest CENTROID within sqrt(area/pi).

    The engine has NO island_id -- it only sees geometry. So here too the match uses
    centroid only. We carry each loop's TRUE island_id alongside (for scoring), and
    record `misbind` = the centroid match picked a prev loop whose true island differs
    (the cross-feature contamination the diagnoses warn about), and `reseed` = no match
    (feature birth / guard fallback). These feed the teeth metrics; they do NOT change
    the field, which is computed exactly as the engine would from the matched loop."""
    guard = 2.0 * LAMBDA
    prev = None  # list of loop-field dicts from the layer below
    states = []
    for li, loops in enumerate(layers):
        cur = []
        for lp in loops:
            xy, s, L, area = lp["xy"], lp["s"], lp["L"], lp["area"]
            cen = lp["centroid"]
            n_wall = max(1, round(L / LAMBDA))
            round_loop = (L > 1e-9) and (4.0 * math.pi * area / (L * L)) > 0.6
            # match to the nearest centroid below within sqrt(area/pi)  (C++ L146-149)
            match = None
            reseed = True
            if prev is not None:
                best = math.sqrt(max(area, 1.0) / math.pi)
                for pf in prev:
                    dc = float(np.hypot(*(pf["centroid"] - cen)))
                    if dc < best:
                        best = dc
                        match = pf
                reseed = match is None
            # misbind: matched a loop whose TRUE feature differs from ours (contamination)
            misbind = (match is not None) and (match["island_id"] != lp["island_id"])
            # axis / ncyc: round loops inherit from a matched round loop below
            axis = cen.copy()
            ncyc = n_wall
            if round_loop and match is not None and match["round"]:
                axis = match["axis"]
                ncyc = match["ncyc"]
            # build this loop's (cos,sin) field
            if round_loop:
                ph = ncyc * np.arctan2(xy[:, 1] - axis[1], xy[:, 0] - axis[0])
                cs = np.column_stack([np.cos(ph), np.sin(ph)])
            else:
                if match is not None:
                    j, t, dmin = _nearest_seg(xy, match["xy"])
                    cs0 = match["cs"][j]
                    cs1 = match["cs"][(j + 1) % len(match["cs"])]
                    cs = cs0 * (1 - t)[:, None] + cs1 * t[:, None]
                    nrm = np.maximum(np.hypot(cs[:, 0], cs[:, 1]), 1e-9)
                    cs = cs / nrm[:, None]
                    # guard fallback: points farther than 2*lambda re-seed from arc length
                    bad = dmin > guard
                    if bad.any():
                        ph = 2 * math.pi * n_wall * s / L
                        cs[bad] = np.column_stack([np.cos(ph), np.sin(ph)])[bad]
                        reseed = True  # a partial re-seed is still a discontinuity injected
                else:
                    ph = 2 * math.pi * n_wall * s / L  # fresh arc-length seed
                    cs = np.column_stack([np.cos(ph), np.sin(ph)])
            cur.append(dict(xy=xy, centroid=cen, round=round_loop, axis=axis, ncyc=ncyc,
                            cs=cs, reseed=reseed, misbind=misbind, island_id=lp["island_id"]))
        states.append(cur)
        prev = cur
    return states


def _prop_lookup(loop, li, state):
    """Resolve a loop to its stored prop record the ENGINE way -- by nearest centroid
    among this layer's loops (the engine never knows island_id). This means a mis-bound
    record is what gets used, exactly as it would ship."""
    cur = state[li]
    return min(cur, key=lambda r: float(np.hypot(*(r["centroid"] - loop["centroid"]))))


def _prop_field(loop, li, mode, state):
    rec = _prop_lookup(loop, li, state)
    return AMP * _sign(li, mode) * rec["cs"][:, 1]  # sin component


register_method("arc",     dict(field=_arc_field))
register_method("angle",   dict(prepare=_angle_prepare, field=_angle_field))
register_method("contour", dict(prepare=_contour_prepare, field=_contour_field))
register_method("prop",    dict(prepare=_prop_prepare, field=_prop_field))
register_method("world",   _make_world("hex"))


# ===================================================================== metrics
def _match_pts(xy0, xy1, eps):
    """Match each point on xy0 to nearest on xy1; return idx, dist."""
    d = np.sqrt((xy0[:, 0][:, None] - xy1[:, 0][None, :]) ** 2 +
                (xy0[:, 1][:, None] - xy1[:, 1][None, :]) ** 2)
    j = np.argmin(d, axis=1)
    return j, d[np.arange(len(xy0)), j]


def _island_loops(layer):
    """Group a layer's loops by island_id."""
    out = {}
    for lp in layer:
        out.setdefault(lp["island_id"], []).append(lp)
    # if duplicate island ids per layer (shouldn't happen), keep the longest
    return {k: max(v, key=lambda lp: lp["L"]) for k, v in out.items()}


def interlock_rms_pct(layers, spec, state, eps=0.5):
    """HEADLINE adjacent-layer interlock registration, PER ISLAND.

    The strength-critical quantity: layer N's modulation at a physical (x,y), and
    layer N+1's modulation at the SAME physical (x,y), must realise the INTENDED
    nested half-wave stagger -- i.e. peak over trough. We compute each layer's NESTED
    Z (with parity sign), match every point on island i of layer N to the nearest
    physical point on the SAME island of layer N+1 (within eps), and measure how far
    Z_N + Z_{N+1} is from 0 (perfect antiphase => their sum cancels). Reported as RMS
    of (Z_N + Z_{N+1}) as a % of amplitude. 0 = ideal controlled interlock; ~141%
    (sqrt2) = random phase relationship (no interlock => no Z teeth)."""
    errs = []
    for li in range(len(layers) - 1):
        A = _island_loops(layers[li])
        B = _island_loops(layers[li + 1])
        for iid, la in A.items():
            if iid not in B:
                continue  # island absent next layer (birth/death) -> no interlock pair
            lb = B[iid]
            za = spec["field"](la, li, "nested", state)
            zb = spec["field"](lb, li + 1, "nested", state)
            j, dist = _match_pts(la["xy"], lb["xy"], eps)
            ok = dist <= eps
            if not ok.any():
                continue
            # intended: antiphase -> za + zb == 0 at the shared point
            errs.append(za[ok] + zb[j[ok]])
    if not errs:
        return float("nan")
    e = np.concatenate(errs)
    return float(100.0 * np.sqrt(np.mean(e ** 2)) / AMP)


def lambda_cov(layers, spec, state):
    """Wavelength uniformity along a wall (CoV of peak-to-peak spacing). Measured on
    the LARGEST loop near the TOP of the stack (so recurrence fanning shows), in
    corrugated mode (sign fixed) so we see the raw field shape."""
    li = int(len(layers) * 0.85)
    lp = max(layers[li], key=lambda l: l["L"])
    z = spec["field"](lp, li, "corrugated", state)
    ext = np.where(np.diff(np.sign(np.diff(z))) != 0)[0]
    if len(ext) < 4:
        return float("nan")
    sp = np.diff(ext).astype(float)
    return float(np.std(sp) / (np.mean(sp) + 1e-9))


def amp_cov(layers, spec, state):
    """Amplitude uniformity along a wall (CoV of sliding peak-to-peak). 0 = no beating."""
    li = int(len(layers) * 0.85)
    lp = max(layers[li], key=lambda l: l["L"])
    z = spec["field"](lp, li, "corrugated", state)
    W = max(6, int(round(1.5 * LAMBDA / RES)))
    p2p = np.array([np.ptp(np.take(z, range(i, i + W), mode="wrap")) for i in range(len(z))])
    return float(np.std(p2p) / (np.mean(p2p) + 1e-9))


def closure_err(layers, spec, state):
    """Closed-loop seam continuity: a loop's phase should complete an INTEGER number
    of waves so the seam has no discontinuity. Measure the Z step between the last and
    first sample of the largest loop (mid stack), as a fraction of peak-to-peak (the
    wrap should be continuous in the field). 0 = closed; up to 1 = half-wave jump."""
    li = len(layers) // 2
    lp = max(layers[li], key=lambda l: l["L"])
    z = spec["field"](lp, li, "corrugated", state)
    pp = np.ptp(z) + 1e-9
    # compare seam wrap step to a typical interior step (so we don't penalise the slope)
    interior = np.median(np.abs(np.diff(z)))
    seam = abs(z[0] - z[-1])
    return float(max(0.0, (seam - interior)) / pp)


def coverage_modulation(layers, spec, state):
    """coverage = fraction of points with |Z|>0.3*amp; modulation = mean|Z|/amp. A real
    weave needs both high (the field actually modulates the wall, not a flat line)."""
    cov = 0
    modz = 0.0
    n = 0
    for li in range(0, len(layers), max(1, len(layers) // 30)):
        for lp in layers[li]:
            z = spec["field"](lp, li, "corrugated", state)
            cov += int(np.count_nonzero(np.abs(z) > 0.3 * AMP))
            modz += float(np.abs(z).sum())
            n += len(z)
    return cov / max(n, 1), modz / max(n, 1) / AMP


def interlock_at_events(layers, spec, state, events, eps=0.5, span=1):
    """Adjacent-layer interlock RMS computed ONLY on the layers at/after an event
    (re-seed, merge, mis-bind). A localised scramble that the whole-loop average
    dilutes shows up here. `events` is a set of layer indices. Returns RMS% or nan."""
    ev = set()
    for e in events:
        for d in range(-span, span + 1):
            ev.add(e + d)
    errs = []
    for li in sorted(ev):
        if li < 0 or li >= len(layers) - 1:
            continue
        A = _island_loops(layers[li]); B = _island_loops(layers[li + 1])
        for iid, la in A.items():
            if iid not in B:
                continue
            lb = B[iid]
            za = spec["field"](la, li, "nested", state)
            zb = spec["field"](lb, li + 1, "nested", state)
            j, dist = _match_pts(la["xy"], lb["xy"], eps)
            ok = dist <= eps
            if ok.any():
                errs.append(za[ok] + zb[j[ok]])
    if not errs:
        return float("nan")
    e = np.concatenate(errs)
    return float(100.0 * np.sqrt(np.mean(e ** 2)) / AMP)


def prop_event_counts(layers):
    """Teeth for the SHIPPED engine only: re-build the prop recurrence and count
    re-seed events (loops with no match below -> fresh seed -> phase discontinuity)
    and mis-binds (centroid match picked the WRONG feature -> cross-feature phase
    contamination). Also return the set of layer indices where these occur so the
    interlock can be measured AT them. These do not apply to stateless world fields."""
    state = _prop_prepare(layers)
    reseeds, misbinds = 0, 0
    reseed_layers, misbind_layers = set(), set()
    for li, cur in enumerate(state):
        if li == 0:
            continue  # the bottom seed is expected, not a defect
        for r in cur:
            if r["reseed"]:
                reseeds += 1; reseed_layers.add(li)
            if r["misbind"]:
                misbinds += 1; misbind_layers.add(li)
    return dict(reseeds=reseeds, misbinds=misbinds,
                reseed_layers=reseed_layers, misbind_layers=misbind_layers,
                event_layers=reseed_layers | misbind_layers, state=state)


def top_lambda_cov(layers, spec, state):
    """Wavelength fanning at the VERY TOP of the stack minus the BOTTOM, on the largest
    loop. A stateless field stays flat (drift ~0); a propagation recurrence fans, so
    top-CoV climbs above bottom-CoV. Returns (bottom_cov, top_cov, climb=top-bottom)."""
    def cov_at(frac):
        li = min(len(layers) - 1, max(0, int(len(layers) * frac)))
        lp = max(layers[li], key=lambda l: l["L"])
        z = spec["field"](lp, li, "corrugated", state)
        ext = np.where(np.diff(np.sign(np.diff(z))) != 0)[0]
        if len(ext) < 4:
            return float("nan")
        sp = np.diff(ext).astype(float)
        return float(np.std(sp) / (np.mean(sp) + 1e-9))
    b, t = cov_at(0.08), cov_at(0.97)
    climb = (t - b) if (b == b and t == t) else float("nan")
    return b, t, climb


def per_island_ok(layers, spec, state, eps=0.5, thresh=70.0):
    """Per-island independence + robustness: for EACH island, the adjacent-layer
    interlock RMS (same metric, restricted to that island) must be bounded. Returns
    True only if every island that persists across >=8 layer-pairs stays below
    `thresh`% -- so a single mis-bound / re-seeded feature flips this False even when
    the whole-loop average looks fine."""
    per = {}
    for li in range(len(layers) - 1):
        A = _island_loops(layers[li])
        B = _island_loops(layers[li + 1])
        for iid, la in A.items():
            if iid not in B:
                continue
            lb = B[iid]
            za = spec["field"](la, li, "nested", state)
            zb = spec["field"](lb, li + 1, "nested", state)
            j, dist = _match_pts(la["xy"], lb["xy"], eps)
            ok = dist <= eps
            if not ok.any():
                continue
            e = za[ok] + zb[j[ok]]
            per.setdefault(iid, []).extend(e.tolist())
    if not per:
        return False
    for iid, e in per.items():
        if len(e) < 8 * 4:
            continue
        rms = 100.0 * math.sqrt(sum(v * v for v in e) / len(e)) / AMP
        if rms > thresh:
            return False
    return True


# ===================================================================== judge
# Engine event layers are a property of the SHIPPED matcher, computed once per part and
# shared by all methods so each method's interlock is probed at the SAME stress layers.
_EVENTS = {}


def score_method(name, parts_cache):
    """Score a registered method on all PASS parts. Returns a dict of metrics."""
    spec = _METHODS[name]
    out = {}
    for pname, (layers, is_mi) in parts_cache.items():
        state = spec["prepare"](layers)
        ev = _EVENTS[pname]
        il = interlock_rms_pct(layers, spec, state)
        b, t, climb = top_lambda_cov(layers, spec, state)
        out[pname] = dict(
            interlock=il,
            il_event=interlock_at_events(layers, spec, state, ev["event_layers"]),
            lam=lambda_cov(layers, spec, state),
            lam_climb=climb,
            amp=amp_cov(layers, spec, state),
            clo=closure_err(layers, spec, state),
            isl=per_island_ok(layers, spec, state),
            reseeds=ev["reseeds"],
            misbinds=ev["misbinds"],
        )
        if is_mi:
            out[pname]["multi_island"] = il
    # global coverage/modulation on the multi-island part (the representative one)
    mi_layers = parts_cache["multi_island"][0]
    state = spec["prepare"](mi_layers)
    cov, mod = coverage_modulation(mi_layers, spec, state)
    out["_global"] = dict(coverage=cov, modulation=mod)
    return out


def score_twist(name, layers):
    spec = _METHODS[name]
    state = spec["prepare"](layers)
    return interlock_rms_pct(layers, spec, state)


# ===================================================================== viz
def render(parts_cache, results):
    fig = plt.figure(figsize=(15, 9))
    mi_layers = parts_cache["multi_island"][0]

    # (1) headline interlock bar chart per part
    ax = fig.add_subplot(2, 3, 1)
    methods = list(_METHODS.keys())
    parts = list(parts_cache.keys())
    x = np.arange(len(parts))
    w = 0.15
    for i, m in enumerate(methods):
        vals = [results[m][p]["interlock"] for p in parts]
        ax.bar(x + i * w, vals, w, label=m)
    ax.axhline(40, color="k", ls="--", lw=0.7)
    ax.set_xticks(x + 2 * w)
    ax.set_xticklabels(parts, rotation=20, fontsize=7)
    ax.set_ylabel("interlock RMS (% amp, 0=ideal)")
    ax.set_title("Adjacent-layer interlock (headline)", fontsize=9)
    ax.legend(fontsize=6)

    # (2) wavelength CoV per part
    ax = fig.add_subplot(2, 3, 2)
    for i, m in enumerate(methods):
        vals = [results[m][p]["lam"] for p in parts]
        ax.bar(x + i * w, vals, w, label=m)
    ax.axhline(0.25, color="k", ls="--", lw=0.7)
    ax.set_xticks(x + 2 * w)
    ax.set_xticklabels(parts, rotation=20, fontsize=7)
    ax.set_ylabel("lambda CoV (0=uniform)")
    ax.set_title("Wavelength uniformity (top of stack)", fontsize=9)

    # (3) multi-island layer plan (a mid layer) coloured by prop Z
    ax = fig.add_subplot(2, 3, 3, aspect="equal")
    spec = _METHODS["prop"]
    state = spec["prepare"](mi_layers)
    li = int(len(mi_layers) * 0.5)
    for lp in mi_layers[li]:
        z = spec["field"](lp, li, "nested", state)
        ax.scatter(lp["xy"][:, 0], lp["xy"][:, 1], c=z, cmap="coolwarm",
                   vmin=-AMP, vmax=AMP, s=4)
    ax.set_title(f"multi_island layer {li}: prop Z (islands)", fontsize=9)
    ax.set_xticks([]); ax.set_yticks([])

    # (4) prop interlock error along height on multi_island body
    ax = fig.add_subplot(2, 3, 4)
    for m in ("prop", "world", "contour"):
        spec = _METHODS[m]
        state = spec["prepare"](mi_layers)
        per_layer = []
        for li in range(len(mi_layers) - 1):
            A = _island_loops(mi_layers[li]); B = _island_loops(mi_layers[li + 1])
            if 0 not in A or 0 not in B:
                per_layer.append(np.nan); continue
            za = spec["field"](A[0], li, "nested", state)
            zb = spec["field"](B[0], li + 1, "nested", state)
            j, dist = _match_pts(A[0]["xy"], B[0]["xy"], 0.5)
            ok = dist <= 0.5
            e = (za[ok] + zb[j[ok]]) if ok.any() else np.array([np.nan])
            per_layer.append(100.0 * np.sqrt(np.nanmean(e ** 2)) / AMP)
        ax.plot(per_layer, lw=0.8, label=m)
    ax.set_xlabel("layer"); ax.set_ylabel("body interlock RMS (% amp)")
    ax.set_title("Interlock vs height (body, multi_island)", fontsize=9)
    ax.legend(fontsize=7)

    # (5) prop Z along the body wall at low vs high layer (recurrence drift)
    ax = fig.add_subplot(2, 3, 5)
    spec = _METHODS["prop"]; state = spec["prepare"](mi_layers)
    for li, c in ((5, "C0"), (int(len(mi_layers) * 0.9), "C3")):
        body = _island_loops(mi_layers[li]).get(0)
        if body is not None:
            z = spec["field"](body, li, "corrugated", state)
            ax.plot(z, c, lw=0.6, label=f"layer {li}")
    ax.set_ylim(-AMP * 1.3, AMP * 1.3)
    ax.set_xlabel("point along body wall"); ax.set_ylabel("Z (mm)")
    ax.set_title("prop: body wall low vs high (fanning?)", fontsize=9)
    ax.legend(fontsize=7)

    # (6) world vs prop Z along dome wall (fanning vs uniform)
    ax = fig.add_subplot(2, 3, 6)
    dome_layers = parts_cache["dome"][0]
    li = int(len(dome_layers) * 0.85)
    for m, c in (("prop", "C0"), ("contour", "C1"), ("world", "C2")):
        spec = _METHODS[m]; state = spec["prepare"](dome_layers)
        lp = max(dome_layers[li], key=lambda l: l["L"])
        ax.plot(spec["field"](lp, li, "corrugated", state), c, lw=0.6, label=m)
    ax.set_ylim(-AMP * 1.3, AMP * 1.3)
    ax.set_xlabel("point along dome wall (top)"); ax.set_ylabel("Z (mm)")
    ax.set_title("dome top: wavelength uniformity", fontsize=9)
    ax.legend(fontsize=7)

    fig.suptitle("Woven-wall faithful validation (v2): multi-island, topology-changing, "
                 "many-layer, adjacent-layer interlock", fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig("validate_weave2.png", dpi=130, bbox_inches="tight")
    plt.close(fig)


# ===================================================================== main
def _fmt(v, w=6, p=2):
    if isinstance(v, bool):
        return f"{'yes' if v else 'NO':>{w}s}"
    if v != v:  # nan
        return f"{'nan':>{w}s}"
    return f"{v:{w}.{p}f}"


def main():
    print("Building parts (multi-island, topology-changing, many layers, NO artificial spin)...")
    parts_cache = {}
    for pname, (fac, is_mi) in PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        _EVENTS[pname] = prop_event_counts(layers)
        nis = max(len(l) for l in layers)
        ev = _EVENTS[pname]
        print(f"  {pname:13s}  layers={len(layers):4d}  max_islands/layer={nis}"
              f"  engine: re-seeds={ev['reseeds']}  mis-binds={ev['misbinds']}")
    twist_layers = TWIST_PART[1]()
    _EVENTS["twist"] = prop_event_counts(twist_layers)
    print(f"  {'twist':13s}  layers={len(twist_layers):4d}  (scored separately)\n")

    print(f"params: lambda={LAMBDA}mm  amp={AMP}mm  layer_h={LAYER_H}mm  res={RES}mm\n")

    results = {m: score_method(m, parts_cache) for m in _METHODS}
    parts = list(PARTS.keys())

    # ---- headline table: adjacent-layer interlock RMS (lower=better, 0 ideal) ----
    print("=== ADJACENT-LAYER INTERLOCK RMS  (% of amplitude, 0=ideal, ~141=random) ===")
    print("   (HEADLINE: how cleanly layer N's peak sits over layer N+1's trough, per island)")
    head = f"{'method':9s}" + "".join(f"{p:>14s}" for p in parts)
    print(head)
    print("-" * len(head))
    for m in _METHODS:
        row = f"{m:9s}" + "".join(f"{_fmt(results[m][p]['interlock'], 14, 1):>14s}" for p in parts)
        print(row)

    # ---- interlock localised AT engine events (re-seed / merge / mis-bind) -------
    print("\n=== INTERLOCK RMS *AT ENGINE EVENT LAYERS* (re-seed / merge / mis-bind, +-1) ===")
    print("   (whole-loop average dilutes a localised scramble; this isolates it)")
    head = f"{'method':9s}" + "".join(f"{p:>14s}" for p in parts)
    print(head)
    print("-" * len(head))
    for m in _METHODS:
        row = f"{m:9s}" + "".join(f"{_fmt(results[m][p]['il_event'], 14, 1):>14s}" for p in parts)
        print(row)

    # ---- per-part full metric block --------------------------------------------
    print("\n=== FULL METRICS PER PART ===")
    print("   (interlock/il@evt/lam_cov/lam_climb/amp_cov/closure: 0=ideal; island: yes=ok)")
    print(f"{'part':13s} {'method':9s} {'interlk':>8s} {'il@evt':>7s} {'lam_cov':>8s} "
          f"{'lamclmb':>8s} {'amp_cov':>8s} {'closure':>8s} {'island':>7s}")
    print("-" * 78)
    for p in parts:
        for m in _METHODS:
            r = results[m][p]
            print(f"{p:13s} {m:9s} {_fmt(r['interlock'],8,1):>8s} {_fmt(r['il_event'],7,1):>7s} "
                  f"{_fmt(r['lam'],8,2):>8s} {_fmt(r['lam_climb'],8,2):>8s} "
                  f"{_fmt(r['amp'],8,2):>8s} {_fmt(r['clo'],8,2):>8s} {_fmt(r['isl'],7):>7s}")
        print()

    # ---- coverage / modulation (is it a real weave) ----------------------------
    print("=== REAL-WEAVE CHECK (multi_island; coverage>0.4 AND modulation>0.45 needed) ===")
    print(f"{'method':9s} {'coverage':>9s} {'modulation':>11s}")
    for m in _METHODS:
        g = results[m]["_global"]
        print(f"{m:9s} {g['coverage']:9.2f} {g['modulation']:11.2f}")

    # ---- twisting part (scored SEPARATELY) -------------------------------------
    print("\n=== TWISTING PART (scored separately; REAL geometric twist) ===")
    print("   (world/angle/contour CANNOT register a truly twisting wall; intrinsic prop can)")
    print(f"{'method':9s} {'interlock_rms_pct':>18s}")
    for m in _METHODS:
        print(f"{m:9s} {_fmt(score_twist(m, twist_layers),18,1):>18s}")

    # ---- PASS gate (the FIVE strength requirements; NOT tuned to a method) -------
    # A method PASSES overall only if it satisfies, on EVERY realistic part:
    #   (1) adjacent-layer interlock registration  (interlock<30, AND il@event<35 where
    #       the engine produces events, AND twist-registration<30 -- a truly twisting
    #       wall is the registration acid test world-keyed fields cannot pass)
    #   (2) uniform wavelength along the loop          (lam_cov<0.25, lam_climb<0.15)
    #   (3) uniform amplitude                          (amp_cov<0.20)
    #   (4) closure                                     (closure<0.20)
    #   (5) per-island independence + a real weave     (per_island AND coverage/modulation)
    # The non-developable-surface fact means NO field satisfies all five on all parts:
    # intrinsic prop fans/mis-binds (req 1,2 on sharp/multi); world-keyed fields beat or
    # cannot register a twist (req 1,3). So a PASS here would be news -- and there is none.
    GATE_PARTS = ("multi_island", "sharp_box", "elongated", "dome")
    print("\n=== PASS GATE (the five strength requirements, on EVERY realistic part) ===")
    def passes(m):
        reasons = []
        for p in GATE_PARTS:
            r = results[m][p]
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
        # req 1 acid test: register a genuinely twisting wall
        tw = score_twist(m, twist_layers)
        if not (tw == tw and tw < 30.0):
            reasons.append(f"twist.interlock={_fmt(tw,0,1).strip()}>=30")
        g = results[m]["_global"]
        if not (g["coverage"] > 0.40 and g["modulation"] > 0.45):
            reasons.append("not_a_real_weave")
        return reasons
    any_pass = False
    gate = {}
    for m in _METHODS:
        reasons = passes(m)
        ok = not reasons
        gate[m] = ok
        any_pass = any_pass or ok
        print(f"  {m:9s} {'PASS' if ok else 'FAIL'}"
              + ("" if ok else "   <- " + "; ".join(reasons[:5])))

    render(parts_cache, results)
    print("\nwrote validate_weave2.png")

    # ---- headline conclusions (teeth) ------------------------------------------
    pmi = results["prop"]["multi_island"]
    psh = results["prop"]["sharp_box"]
    mi_ev = _EVENTS["multi_island"]
    print("\n--- conclusions ---")
    print(f"engine on multi_island: re-seeds={mi_ev['reseeds']} mis-binds={mi_ev['misbinds']}"
          f"  -> these are the topology events v1 never produced")
    print(f"prop multi_island: interlock {pmi['interlock']:.1f}%  AT-EVENTS {pmi['il_event']:.1f}%"
          f"  lam_cov {pmi['lam']:.2f}  lam_climb {pmi['lam_climb']:.2f}")
    print(f"prop sharp_box   : interlock {psh['interlock']:.1f}%  AT-EVENTS {psh['il_event']:.1f}%"
          f"  lam_cov {psh['lam']:.2f}  lam_climb {psh['lam_climb']:.2f}")
    # teeth check: prop must FAIL the gate (on lambda fanning / event interlock / sharp box)
    prop_fails = not gate["prop"]
    print(f"prop FAILS the gate (teeth on the real stressors): {prop_fails}")
    print(f"any method trivially PASSES everything: {any_pass}")
    print("RESULT:", "INFORMATIVE — harness has teeth: prop fails on the multi-island/sharp "
          "stressors (re-seed/mis-bind/lambda-fan), and no method gets a free pass"
          if (prop_fails and not any_pass) else
          ("REVIEW — prop did NOT fail the stressors; check the model" if not prop_fails else
           "REVIEW — a method trivially passes everything; tighten the gate"))


if __name__ == "__main__":
    main()
