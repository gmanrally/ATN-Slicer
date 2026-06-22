#!/usr/bin/env python3
"""Adversarial attack on candidate 'anchored_arclen_curv'.

The candidate's ENTIRE adjacent-layer registration (req 1) rests on _anchor_u_xray:
seed the seam at the +x-ray crossing from the centroid, taking cross[0] (the FIRST
crossing in VERTEX/SEAM order). The harness rolls the seam RANDOMLY each layer.

That is fine ONLY IF the +x ray crosses the loop EXACTLY ONCE. The instant a
physically-real loop has a re-entrant feature on its +x flank (a cooling slot, a
mounting-ear gap, a pinched waist, a velocity-stack throat) the +x ray crosses
2+ times, so 'cross[0]' depends on the random seam roll -> the anchor teleports
between adjacent layers -> registration scrambles. Centroid drift across a corner /
an island split do the same to the centroid the ray is cast from.

We build several such PHYSICALLY-REAL parts, score the candidate through the SAME
judge metrics, and compare against the harness's own baselines.
"""
import math
import numpy as np
import validate_weave2 as J
import importlib.util

# import the candidate module (hyphenated filename) to register its method
spec = importlib.util.spec_from_file_location(
    "cand_anchored", "cand_anchored-arclen-curvature.py")
cand = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cand)
METHOD = cand.METHOD_NAME  # "anchored_arclen_curv"

SEED = J.SEED
RES = J.RES


# ---------------------------------------------------------------- geometry helpers
def _slotted_disc(cx, cy, R, slot_w, slot_depth, n=360):
    """A round body (a velocity-stack collar / a cooling boss) with a RE-ENTRANT
    radial SLOT cut into its +x flank (theta ~ 0). Physically real: a split-collar
    gap, a wire channel, a cooling slit. The slot makes the +x ray from the centroid
    cross the boundary at MORE THAN ONE point near theta=0 -> _anchor_u_xray's cross[0]
    is seam-order dependent."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    r = np.full(n, R, float)
    # carve a notch around theta=0: a smooth inward dip of width ~slot_w (radians)
    ang = np.mod(th + math.pi, 2 * math.pi) - math.pi  # in (-pi, pi], 0 at +x
    half = slot_w / 2.0
    inside = np.abs(ang) < half
    # inside the slot, pull radius inward by slot_depth with a flat-ish bottom
    r = np.where(inside, R - slot_depth * (0.5 + 0.5 * np.cos(math.pi * ang / half)), r)
    x = cx + r * np.cos(th)
    y = cy + r * np.sin(th)
    return np.column_stack([x, y])


def _dumbbell(cx, cy, R, sep, neck, n=360):
    """Two round lobes joined by a thin neck along x (a part that is about to SPLIT
    into two islands, or just merged from two). The centroid sits in the neck; the +x
    ray crosses the right lobe AND can graze the neck. As `sep` grows the waist pinches
    to a near-cusp then splits. Physically real: twin velocity stacks merging into a
    plenum, a figure-8 duct."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    # superformula-ish: radius modulated so two bulges along +-x with a thin waist
    ang = th
    # base lobes
    lobe = R * (1.0 + 0.0 * np.cos(ang))
    # waist factor: squeeze near theta=pi/2 and 3pi/2 (top/bottom) to make an x-elongated
    # peanut; sep controls lobe separation, neck controls waist thinness
    x = (sep + R * np.cos(ang)) * np.sign(np.cos(ang)) * 0 + (R + sep) * np.cos(ang)
    waist = neck + (1 - neck) * np.abs(np.cos(ang))  # ~neck at top/bottom, 1 at sides
    y = R * waist * np.sin(ang)
    return np.column_stack([cx + x, cy + y])


def _peanut(cx, cy, A, B, waist, n=360):
    """A peanut/waisted loop pinched along its middle (x). waist in [0,1]: 1 = ellipse,
    ->0 = pinches to a near-cusp at the centre. The pinch sits ON the +x... no: the
    waist is at x=0 (the centre). To put a pinch on the +x flank we offset. Used for the
    near-degenerate thin-section test."""
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    x = A * np.cos(th)
    # vertical half-extent collapses near x=0 (theta=+-pi/2) -> waisted
    pinch = waist + (1 - waist) * (np.cos(th) ** 2)
    y = B * pinch * np.sin(th)
    return np.column_stack([cx + x, cy + y])


def _make_layers_from(gen, nlayers, seed_off=0, multi=False):
    """gen(k, nlayers) -> list of (island_id, xy). Wrap into harness loop-dicts with
    the harness's own random seam roll (so we test seam sensitivity honestly)."""
    rng = np.random.default_rng(SEED + 50 + seed_off)
    layers = []
    for k in range(nlayers):
        loops = gen(k, nlayers)
        layers.append([J._loop(xy, iid, rng) for (iid, xy) in loops])
    return layers


# ---------------------------------------------------------------- adversarial parts
def part_slotted_collar(nlayers=120):
    """A round collar with a re-entrant +x slot whose DEPTH grows then recedes with
    height (a real cooling slit that opens up a section then closes). The slot is deep
    enough over the mid-section that the +x ray crosses the wall 3x."""
    def gen(k, N):
        z = k / N
        R = 14.0 + 2.0 * math.sin(math.pi * z)
        depth = 9.0 * max(0.0, math.sin(math.pi * min(1.0, max(0.0, (z - 0.15) / 0.7))))
        w = 0.9  # slot angular width (rad)
        return [(0, _slotted_disc(0, 0, R, w, depth,
                                  n=max(120, int(2 * math.pi * R / RES))))]
    return _make_layers_from(gen, nlayers)


def part_offcentre_slot(nlayers=120):
    """Same idea but the slot DRIFTS in angle across +x with height, and the body is
    OVAL (so it is developable, exercising the arc branch not the round branch). This
    moves the multi-crossing region in and out of the +x ray -> the worst case for a
    fixed +x anchor."""
    def gen(k, N):
        z = k / N
        a = 16.0 + 2.0 * math.sin(math.pi * z)
        b = 11.0
        th = np.linspace(0, 2 * math.pi, 320, endpoint=False)
        x = a * np.cos(th); y = b * np.sin(th)
        # re-entrant slot centred at angle phi (drifts), depth fixed deep
        phi = math.radians(-12 + 24 * z)  # sweeps across +x
        ang = np.mod(th - phi + math.pi, 2 * math.pi) - math.pi
        half = 0.55
        inside = np.abs(ang) < half
        depth = 7.0
        rad = np.hypot(x, y)
        # pull those vertices inward radially
        fac = np.where(inside, 1.0 - (depth / np.maximum(rad, 1e-6)) *
                       (0.5 + 0.5 * np.cos(math.pi * ang / half)), 1.0)
        return [(0, np.column_stack([x * fac, y * fac]))]
    return _make_layers_from(gen, nlayers)


def part_splitting_island(nlayers=120):
    """One body that PHYSICALLY SPLITS into two islands partway up (a Y-shaped duct, a
    plenum that branches into two runner stacks). Below the split it's a single waisted
    loop; above, two separate ellipses with DISTINCT island ids. Tests centroid jump +
    feature birth + per-island independence at a real topology change."""
    split_at = nlayers // 2
    def gen(k, N):
        z = k / N
        if k < split_at:
            # single peanut, waist thinning toward the split
            f = k / max(1, split_at)
            waist = 0.85 - 0.7 * f  # -> ~0.15 just before split (near-cusp)
            xy = _peanut(0, 0, 18.0, 9.0, max(0.08, waist), n=300)
            return [(0, xy)]
        else:
            # two ellipses drifting apart; ids 0 (left) and 1 (right)
            f = (k - split_at) / max(1, N - split_at)
            dx = 6.0 + 8.0 * f
            left = J._ellipse(-dx, 0, 7.0, 8.0, n=160)
            right = J._ellipse(dx, 0, 7.0, 8.0, n=160)
            return [(0, left), (1, right)]
    return _make_layers_from(gen, nlayers, multi=True)


def part_pinch_cusp(nlayers=120):
    """A loop that pinches to a near-cusp on its +x flank (a re-entrant near-zero-radius
    notch), the cusp depth oscillating with height. Near a cusp the centroid barely
    moves but the +x crossing structure flips between 1 and 3 crossings repeatedly."""
    def gen(k, N):
        z = k / N
        th = np.linspace(0, 2 * math.pi, 340, endpoint=False)
        R = 13.0
        x = R * np.cos(th); y = R * 0.8 * np.sin(th)
        ang = np.mod(th + math.pi, 2 * math.pi) - math.pi
        # sharp narrow notch on +x; depth oscillates so crossing-count flips each ~few layers
        depth = 8.0 * (0.5 + 0.5 * math.sin(2 * math.pi * z * 4))
        half = 0.30
        inside = np.abs(ang) < half
        fac = np.where(inside, 1.0 - (depth / R) * (0.5 + 0.5 * np.cos(math.pi * ang / half)), 1.0)
        return [(0, np.column_stack([x * fac, y * fac]))]
    return _make_layers_from(gen, nlayers)


ATTACK_PARTS = {
    "slotted_collar": (part_slotted_collar, False),
    "offcentre_slot": (part_offcentre_slot, False),
    "splitting_isl":  (part_splitting_island, True),
    "pinch_cusp":     (part_pinch_cusp, False),
}


# ---------------------------------------------------------------- crossing diagnostics
def crossing_stats(layers):
    """For each layer's largest loop, count how many times the +x ray from the centroid
    crosses the boundary, and record the anchor arc-fraction the candidate picks. Then
    report adjacent-layer anchor JUMP (in wavelengths) -- the direct cause of scramble."""
    fracs = []
    ncross = []
    for layer in layers:
        lp = max(layer, key=lambda l: l["L"])
        xy = lp["xy"]; c = cand._centroid(lp)
        rel = xy - c
        perp = rel[:, 1]; along = rel[:, 0]
        sg = np.sign(perp)
        cross = np.where((sg != np.roll(sg, -1)) & ((along + np.roll(along, -1)) > 0))[0]
        ncross.append(len(cross))
        du = np.hypot(*(np.roll(xy, -1, axis=0) - xy).T)
        U = du.sum()
        u = np.concatenate([[0.0], np.cumsum(du)])[:len(xy)]
        u0 = cand._anchor_u_xray(xy, c, u, du)
        fracs.append((u0 / U) if U > 1e-9 else 0.0)
    fracs = np.array(fracs)
    # adjacent jump in arc-fraction, wrapped to [-0.5,0.5], in wavelengths
    df = np.diff(fracs)
    df = (df + 0.5) % 1.0 - 0.5
    # convert fraction-of-loop to wavelengths: loop holds n_wall waves; use mean loop L
    Lmean = np.mean([max(l, key=lambda x: x["L"])["L"] for l in layers])
    nwav = max(1, round(Lmean / J.LAMBDA))
    jump_waves = np.abs(df) * nwav
    return dict(ncross=np.array(ncross), jump_waves=jump_waves,
                mean_jump=float(np.mean(jump_waves)), max_jump=float(np.max(jump_waves)),
                pct_multi=float(np.mean(np.array(ncross) != 1)))


def main():
    print(f"Candidate under attack: {METHOD}\n")
    spec_m = J._METHODS[METHOD]

    parts_cache = {}
    for pname, (fac, is_mi) in ATTACK_PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)

    print("=== +x-RAY CROSSING DIAGNOSTICS (the anchor's failure mode) ===")
    print(f"{'part':16s} {'mean_ncross':>11s} {'%multi-cross':>12s} "
          f"{'mean_jump(λ)':>13s} {'max_jump(λ)':>12s}")
    for pname, (layers, _) in parts_cache.items():
        cs = crossing_stats(layers)
        print(f"{pname:16s} {np.mean(cs['ncross']):11.2f} {100*cs['pct_multi']:11.1f}% "
              f"{cs['mean_jump']:13.3f} {cs['max_jump']:12.3f}")

    print("\n=== CANDIDATE METRICS ON ADVERSARIAL PARTS ===")
    print(f"{'part':16s} {'interlk':>8s} {'il@evt':>7s} {'lam_cov':>8s} {'lamclmb':>8s} "
          f"{'amp_cov':>8s} {'closure':>8s} {'island':>7s}")
    cand_rows = {}
    for pname, (layers, is_mi) in parts_cache.items():
        state = spec_m["prepare"](layers)
        ev = J._EVENTS[pname]
        il = J.interlock_rms_pct(layers, spec_m, state)
        b, t, climb = J.top_lambda_cov(layers, spec_m, state)
        row = dict(
            interlock=il,
            il_event=J.interlock_at_events(layers, spec_m, state, ev["event_layers"]),
            lam=J.lambda_cov(layers, spec_m, state),
            lam_climb=climb,
            amp=J.amp_cov(layers, spec_m, state),
            clo=J.closure_err(layers, spec_m, state),
            isl=J.per_island_ok(layers, spec_m, state),
        )
        cand_rows[pname] = row
        def f(v, p=3):
            if isinstance(v, bool): return "yes" if v else "NO"
            if v != v: return "nan"
            return f"{v:.{p}f}"
        print(f"{pname:16s} {f(il,1):>8s} {f(row['il_event'],1):>7s} {f(row['lam'],3):>8s} "
              f"{f(row['lam_climb'],3):>8s} {f(row['amp'],3):>8s} {f(row['clo'],3):>8s} "
              f"{f(row['isl']):>7s}")

    # baseline comparison: how do the harness's OWN methods fare on the same parts?
    print("\n=== BASELINE METHODS ON SAME PARTS (interlock RMS %, lower=better) ===")
    base = ["prop", "arc", "contour", "world", "angle", METHOD]
    head = f"{'method':22s}" + "".join(f"{p:>16s}" for p in ATTACK_PARTS)
    print(head); print("-" * len(head))
    for m in base:
        sm = J._METHODS[m]
        cells = []
        for pname, (layers, _) in parts_cache.items():
            st = sm["prepare"](layers)
            il = J.interlock_rms_pct(layers, sm, st)
            cells.append(f"{il:16.1f}" if il == il else f"{'nan':>16s}")
        print(f"{m:22s}" + "".join(cells))

    return cand_rows


if __name__ == "__main__":
    main()
