#!/usr/bin/env python3
"""ADVERSARIAL ATTACK 1: anchor / parameterisation stability for island-tracked-propagation.

We import the FIXED judge (validate_weave2) and the candidate module so the candidate's
method 'island_prop' is registered, then build NEW physically-real pathological parts and
score the candidate through the SAME harness metrics. We are skeptics: we look for the
adjacent-layer interlock RMS to spike, lambda to fan, or per_island to flip NO.

Attack geometries (all physically real for an airbox / velocity-stack part):
  A. SPLIT: one body loop that pinches and SPLITS into two children over a few layers
     (a bifurcating runner). Greedy track matching must hand one child the parent track;
     the other is a birth -> fresh seam -> registration break at the split.
  B. CUSP-PINCH: a loop that pinches to a near-cusp (a teardrop whose tail nearly closes),
     so the centroid sits ON the wall and the centroid-angle seam anchor + circular-LSQ s0
     become unstable.
  C. TANGENT-ANCHOR: an island whose centroid->0deg ray is TANGENT to a long flat edge, so
     the argmin seam point can jump a large arc distance for a tiny morph (s0 jump).
  D. CONCENTRIC: two nearly-concentric loops (an inner stiffening rib inside the body) with
     near-equal centroids -> _lookup / track matching pick the WRONG record.
  E. MERGE-then-SPLIT chatter: two islands that merge then re-split (a pulsing neck).
"""
import math
import numpy as np

import validate_weave2 as J
import importlib.util, sys, os

# import the candidate module (filename has hyphens) so it registers 'island_prop'
_cand_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "cand_island-tracked-propagation.py")
_spec = importlib.util.spec_from_file_location("cand_island_prop", _cand_path)
_cand = importlib.util.module_from_spec(_spec)
sys.modules["cand_island_prop"] = _cand
_spec.loader.exec_module(_cand)

RES = J.RES
SEED = J.SEED


# ----------------------------------------------------------------- geometry helpers
def _circle(cx, cy, r, n=None):
    n = n or max(40, int(2 * math.pi * r / RES))
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    return np.column_stack([cx + r * np.cos(th), cy + r * np.sin(th)])


def _peanut(cx, cy, sep, r, waist, n=300):
    """Two circles of radius r centred +-sep/2 apart on x, blended into one loop whose
    waist (neck half-width) is `waist`. As waist -> 0 the loop pinches toward a split.
    Implemented as a superposition contour |.|: we trace the outer boundary by sampling
    angle and taking the max radius of the two lobes plus a neck term."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    # parametric: radius field of union of two disks, smoothed; use a metaball.
    pts = []
    for a in th:
        # ray from centre; find boundary by the metaball iso-contour f=1
        dx, dy = math.cos(a), math.sin(a)
        lo, hi = 0.01, sep + 3 * r
        for _ in range(40):
            mid = 0.5 * (lo + hi)
            x = mid * dx; y = mid * dy
            f = (r * r) / ((x - sep / 2) ** 2 + y * y + 1e-6) + \
                (r * r) / ((x + sep / 2) ** 2 + y * y + 1e-6) + \
                (waist * waist) / (x * x + y * y + 1e-6)
            if f > 1.0:
                lo = mid
            else:
                hi = mid
        pts.append((cx + lo * dx, cy + lo * dy))
    return np.array(pts)


def _teardrop(cx, cy, R, tail, n=280):
    """A teardrop: round head radius R, pulled to a near-cusp tail of length `tail`. As
    tail grows the centroid migrates toward the tail tip and the loop develops a sharp
    near-cusp corner (sharper than 90deg)."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    # r(theta): R for the head; near theta=0 (tail dir +x) collapse to a cusp via a
    # narrow power-law spike OUTWARD then a near-zero width.
    # Build as a superellipse-ish: x stretched, y pinched near the tail.
    x = R * np.cos(th)
    y = R * np.sin(th)
    # pull +x side into a tail: scale x by (1+tail) on the +x lobe, pinch y by |cos|^p
    pull = 0.5 * (1 + np.cos(th))  # 1 at theta=0, 0 at theta=pi
    x = x + tail * pull ** 2
    y = y * (1.0 - 0.92 * pull ** 3)  # pinch the tail width strongly -> near cusp
    return np.column_stack([cx + x, cy + y])


def _flat_tab(cx, cy, w, h, tabw, n_per=60):
    """A box with a long FLAT top edge and a small tab; the centroid->0deg (+x) ray hits
    a flat edge tangentially. Built as a polygon then resampled."""
    # rectangle w x h centred, with a rectangular notch on +x to make the centroid->+x
    # ray graze a long flat edge.
    hw, hh = w / 2, h / 2
    poly = [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]
    # insert many colinear points along the +x flat edge (right edge) so argmin of the
    # seam-angle can jump along a degenerate flat run
    out = []
    out.append((cx + hw, cy - hh))
    for j in range(1, n_per):
        out.append((cx + hw, cy - hh + h * j / n_per))  # right flat edge, dense
    out.append((cx + hw, cy + hh))
    out.append((cx - hw, cy + hh))
    out.append((cx - hw, cy - hh))
    return np.array(out)


def _ellipse(cx, cy, a, b, n=240, rot=0.0):
    return J._ellipse(cx, cy, a, b, n=n, rot=rot)


def _loop(xy, iid, rng):
    return J._loop(xy, iid, rng)


# ----------------------------------------------------------------- adversarial parts
def part_split(nlayers=120):
    """A single runner body that PINCHES and SPLITS into two velocity-stack roots.
    Layers 0..40 one loop; the waist shrinks; at ~layer 55 it splits into two loops that
    then drift apart and grow. This is a real bifurcation (a Y runner)."""
    rng = np.random.default_rng(SEED + 11)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        if k < 55:
            # one peanut whose waist shrinks toward the split
            waist = 6.5 * (1.0 - k / 55.0) + 0.2
            sep = 16 + 6 * z
            body = _peanut(0, 0, sep, 7.0, waist, n=320)
            loops.append((0, body))
        else:
            # split into two lobes that separate and grow
            f = (k - 55) / max(1, nlayers - 55)
            sep = 22 + 14 * f
            rr = 7.0 + 1.5 * f
            loops.append((1, _circle(-sep / 2, 0, rr)))
            loops.append((2, _circle(+sep / 2, 0, rr)))
        if k % 2:
            loops = loops[::-1]
        layers.append([_loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


def part_cusp(nlayers=110):
    """A teardrop whose tail GROWS into a near-cusp then recedes -> centroid migrates,
    a corner sharper than 90deg appears, the seam anchor wanders. Single island."""
    rng = np.random.default_rng(SEED + 12)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        tail = 18.0 * math.sin(math.pi * z)   # 0 -> 18 -> 0
        td = _teardrop(0, 0, 12.0, tail, n=300)
        layers.append([_loop(td, 0, rng)])
    return layers


def part_tangent(nlayers=100):
    """A box with a long dense flat +x edge; section morphs slightly so the centroid->+x
    seam ray grazes the flat edge -> argmin seam point can jump. Single island."""
    rng = np.random.default_rng(SEED + 13)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        w = 30 + 6 * math.sin(math.pi * z)
        h = 18 + 1.5 * math.sin(2 * math.pi * z + 0.3)
        box = _flat_tab(0, 0, w, h, 0, n_per=70)
        layers.append([_loop(box, 0, rng)])
    return layers


def part_concentric(nlayers=110):
    """Body loop + an inner stiffening RIB nearly concentric with it (same centroid).
    Two loops with near-equal centroids -> nearest-centroid track match / _lookup ambiguous.
    The rib appears at layer 20, grows, stays concentric with the (also growing) body."""
    rng = np.random.default_rng(SEED + 14)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        Rb = 20 + 3 * math.sin(math.pi * z)
        loops.append((0, _ellipse(0.0, 0.0, Rb, Rb * 0.92, n=240)))
        if k >= 20:
            f = (k - 20) / (nlayers - 20)
            Rr = 9 + 5 * f
            # inner rib centroid drifts by < 0.5 mm -> stays within nearest-centroid gate
            cxr = 0.3 * math.sin(3 * z)
            cyr = 0.3 * math.cos(3 * z)
            loops.append((1, _ellipse(cxr, cyr, Rr, Rr * 0.92, n=200)))
        if k % 2:
            loops = loops[::-1]
        layers.append([_loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


def part_chatter(nlayers=120):
    """Two islands whose centroids approach, MERGE (one loop) for a band, then RE-SPLIT.
    A pulsing neck. Track ids should survive a merge-then-split without scrambling."""
    rng = np.random.default_rng(SEED + 15)
    layers = []
    for k in range(nlayers):
        z = k / nlayers
        loops = []
        # gap pulses: separated -> merged -> separated
        phase = math.sin(2.2 * math.pi * z)
        sep = 18 + 14 * phase
        if sep > 9:
            loops.append((1, _circle(-sep / 2, 0, 6.5)))
            loops.append((2, _circle(+sep / 2, 0, 6.5)))
        else:
            loops.append((1, _peanut(0, 0, max(2.0, sep), 6.5, 4.0, n=300)))
        if k % 2:
            loops = loops[::-1]
        layers.append([_loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


ATTACK_PARTS = {
    "split":      part_split,
    "cusp":       part_cusp,
    "tangent":    part_tangent,
    "concentric": part_concentric,
    "chatter":    part_chatter,
}


def score_one(name, layers, is_mi):
    spec = J._METHODS[name]
    # populate engine event cache (used by il_event / gate)
    J._EVENTS["__attack__"] = J.prop_event_counts(layers)
    ev = J._EVENTS["__attack__"]
    state = spec["prepare"](layers)
    il = J.interlock_rms_pct(layers, spec, state)
    b, t, climb = J.top_lambda_cov(layers, spec, state)
    out = dict(
        interlock=il,
        il_event=J.interlock_at_events(layers, spec, state, ev["event_layers"]),
        lam=J.lambda_cov(layers, spec, state),
        lam_climb=climb,
        amp=J.amp_cov(layers, spec, state),
        clo=J.closure_err(layers, spec, state),
        isl=J.per_island_ok(layers, spec, state),
        reseeds=ev["reseeds"],
        misbinds=ev["misbinds"],
    )
    return out


def main():
    name = "island_prop"
    print(f"== ADVERSARIAL ATTACK 1 (anchor/parameterisation) on '{name}' ==\n")
    print(f"{'part':12s} {'interlk':>8s} {'il@evt':>7s} {'lam_cov':>8s} {'lamclmb':>8s} "
          f"{'amp_cov':>8s} {'closure':>8s} {'island':>7s} {'rseed':>5s} {'misb':>5s} "
          f"{'maxisl':>6s}")
    print("-" * 90)
    worst = {}
    for pname, fac in ATTACK_PARTS.items():
        layers = fac()
        is_mi = pname in ("split", "concentric", "chatter")
        r = score_one(name, layers, is_mi)
        nis = max(len(l) for l in layers)
        flag = ""
        if (r["interlock"] != r["interlock"]) or r["interlock"] > 30:
            flag += " <<INTERLOCK"
        if (r["il_event"] == r["il_event"]) and r["il_event"] > 35:
            flag += " <<EVENT"
        if (r["lam"] != r["lam"]) or r["lam"] > 0.25:
            flag += " <<LAMBDA"
        if not r["isl"]:
            flag += " <<ISLAND"
        if (r["clo"] != r["clo"]) or r["clo"] > 0.20:
            flag += " <<CLOSURE"
        print(f"{pname:12s} {J._fmt(r['interlock'],8,1)} {J._fmt(r['il_event'],7,1)} "
              f"{J._fmt(r['lam'],8,2)} {J._fmt(r['lam_climb'],8,2)} {J._fmt(r['amp'],8,2)} "
              f"{J._fmt(r['clo'],8,2)} {J._fmt(r['isl'],7)} {r['reseeds']:5d} {r['misbinds']:5d} "
              f"{nis:6d}{flag}")
        worst[pname] = r
    return worst


if __name__ == "__main__":
    main()
