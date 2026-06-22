#!/usr/bin/env python3
"""ADVERSARIAL attack on candidate 'stateless-anchored-arclen' -- ATTACK ANGLE 1:
anchor / parameterisation stability.

The candidate anchors s=0 at argmax(xy . ANCHOR_DIR), parabola-refined. Its OWN
docstring confesses: where the projection has a flat/degenerate maximum the argmax
can jump discretely between adjacent layers. We construct PHYSICALLY-REAL geometries
that force exactly that, and score the candidate's adjacent-layer interlock (the
strength-critical metric) on them with the SAME judge metrics the harness uses.

We reuse validate_weave2 (J) verbatim for: geometry resample, loop dicts, the
interlock_rms_pct / per_island_ok / lambda_cov metrics, AMP/LAMBDA. We only add NEW
parts. The candidate registers itself in J on import.
"""
import math
import numpy as np
import importlib.util

import validate_weave2 as J

spec = importlib.util.spec_from_file_location("cand", "cand_stateless-anchored-arclen.py")
cand = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cand)

AMP, LAMBDA, RES, SEED = J.AMP, J.LAMBDA, J.RES, J.SEED
ADIR = cand._ANCHOR_DIR
ANG = math.atan2(ADIR[1], ADIR[0])  # ~0.3 rad


# ----------------------------------------------------------------- attack geometries

def _poly_loop(xy, iid, rng, jitter=True):
    return J._loop(xy, iid, rng, jitter_seam=jitter)


def part_twin_peak(nlayers=120):
    """TWO competing maxima along the anchor direction that SWAP dominance.

    A rounded 'peanut'/bilobed body: two rounded lobes joined by a waist, the lobe
    axis aligned with ANCHOR_DIR. The two lobe tips are near-equidistant along
    ANCHOR_DIR; as the part morphs with height, which lobe projects furthest flips
    back and forth. argmax-anchor => s=0 teleports half-way round the loop between
    adjacent layers => the arc parameter (and hence phase) of every physical point
    shifts by ~half the loop => adjacent-layer registration destroyed at the flip
    layers. This is a real shape: think two velocity-stack bells fused into one body,
    or a twin-bore manifold flange.
    """
    rng = np.random.default_rng(SEED + 100)
    layers = []
    n = 300
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    ca, sa = math.cos(ANG), math.sin(ANG)
    for k in range(nlayers):
        z = k / nlayers
        # the two lobe tip distances along ANCHOR_DIR oscillate in antiphase so the
        # argmax flips repeatedly. amplitude ~0.4mm: a tiny, realistic morph.
        d1 = 18.0 + 0.6 * math.sin(2 * math.pi * 6 * z)
        d2 = 18.0 - 0.6 * math.sin(2 * math.pi * 6 * z)
        # build a peanut: radius modulated so two bulges sit at +/- lobe axis
        # base ellipse in lobe frame (u along ANCHOR_DIR, v perpendicular)
        u = np.cos(th)
        v = np.sin(th)
        # bilobe radial profile along u: +tip at u~+1 reaches d1, -tip at u~-1 reaches d2
        rad_u = np.where(u >= 0, d1, d2)
        waist = 9.0  # half-width perpendicular
        x_lf = rad_u * u
        y_lf = waist * v
        # rotate lobe frame into world by ANCHOR_DIR
        x = x_lf * ca - y_lf * sa
        y = x_lf * sa + y_lf * ca
        xy = np.column_stack([x, y])
        layers.append([_poly_loop(xy, 0, rng)])
    return layers


def part_flat_top(nlayers=120):
    """A body with a FLAT edge nearly PERPENDICULAR to ANCHOR_DIR -> a degenerate
    (flat) projection maximum. The argmax wanders along the flat edge between layers;
    the parabola refinement does nothing on a flat plateau (denom~0 -> delta=0) so the
    anchor jumps by whole samples along the flat. Physically: a D-shaped boss, a
    flange with a machined flat, an airbox wall with a mounting pad.
    """
    rng = np.random.default_rng(SEED + 101)
    layers = []
    ca, sa = math.cos(ANG), math.sin(ANG)
    for k in range(nlayers):
        z = k / nlayers
        # D-shape: a half-disc with a straight chord, chord normal = ANCHOR_DIR so the
        # chord (flat) is the far edge along ANCHOR_DIR.
        R = 16.0 + 1.0 * math.sin(math.pi * z)
        # arc part (the round side), angle from +90 round to -90 in lobe frame
        na = 200
        phi = np.linspace(-math.pi / 2, math.pi / 2, na)
        arc_x = R * np.cos(phi)   # >=0 side
        arc_y = R * np.sin(phi)
        # flat chord on the far (+u) side: a vertical flat at x = +d, slightly tilted
        # so its outward normal ~ +u (ANCHOR_DIR). Put the flat at +u so it is the max.
        flat = R * 0.62
        ny = 60
        chord_y = np.linspace(R * math.sin(math.pi / 2), -R * math.sin(math.pi / 2), ny)
        # we want the flat to be the FAR side: place arc on -u, flat on +u
        # rebuild: arc on negative-u semicircle, flat plane on +u
        phi2 = np.linspace(math.pi / 2, 3 * math.pi / 2, na)
        ax = R * np.cos(phi2)   # <=0
        ay = R * np.sin(phi2)
        fy = np.linspace(R, -R, ny)
        fx = np.full(ny, flat) + 0.0 * fy  # vertical flat at x=flat (the far +u edge)
        # tiny micro-tilt that flips sign with height so the argmax end of the flat
        # alternates between the two ends of the flat -> anchor teleports end-to-end.
        tilt = 0.004 * math.sin(2 * math.pi * 7 * z)
        fx = fx + tilt * fy
        x_lf = np.concatenate([ax, fx])
        y_lf = np.concatenate([ay, fy])
        # rotate into world so the flat normal aligns with ANCHOR_DIR
        x = x_lf * ca - y_lf * sa
        y = x_lf * sa + y_lf * ca
        xy = np.column_stack([x, y])
        layers.append([_poly_loop(xy, 0, rng)])
    return layers


def part_near_circle(nlayers=120):
    """A near-CIRCULAR loop (dome-like) with a tiny rotating bump. On a circle the
    projection maximum is well-defined but VERY flat (second derivative ~ R, small
    relative to noise); a sub-mm bump that orbits the loop with height drags the
    argmax around the whole circle -> the anchor sweeps continuously but the phase
    field rotates with it, so a fixed physical point's phase drifts layer to layer
    even though geometry barely moved. Tests whether 'argmax of a flat max' registers.
    """
    rng = np.random.default_rng(SEED + 102)
    layers = []
    n = 260
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    for k in range(nlayers):
        z = k / nlayers
        R = 20.0
        # a small bump whose angular position rotates slowly with height
        bump_ang = 2 * math.pi * 1.5 * z
        bump = 0.5 * np.exp(-((np.mod(th - bump_ang + math.pi, 2 * math.pi) - math.pi) ** 2) / (2 * 0.25 ** 2))
        r = R + bump
        xy = np.column_stack([r * np.cos(th), r * np.sin(th)])
        layers.append([_poly_loop(xy, 0, rng)])
    return layers


# ----------------------------------------------------------------- scoring

def score_part(name, layers, is_mi=False):
    spec = J._METHODS["stateless_anchored_arclen"]
    # build engine events so il_event is computable
    J._EVENTS[name] = J.prop_event_counts(layers)
    state = spec["prepare"](layers)
    il = J.interlock_rms_pct(layers, spec, state)
    b, t, climb = J.top_lambda_cov(layers, spec, state)
    lam = J.lambda_cov(layers, spec, state)
    amp = J.amp_cov(layers, spec, state)
    clo = J.closure_err(layers, spec, state)
    isl = J.per_island_ok(layers, spec, state)
    ev = J._EVENTS[name]
    il_ev = J.interlock_at_events(layers, spec, state, ev["event_layers"])
    # also prop for reference
    pspec = J._METHODS["prop"]
    pstate = pspec["prepare"](layers)
    pil = J.interlock_rms_pct(layers, pspec, pstate)
    return dict(il=il, il_ev=il_ev, lam=lam, lam_climb=climb, amp=amp, clo=clo,
                isl=isl, prop_il=pil)


def anchor_trace(layers, n_show=None):
    """Diagnostic: the anchor arc fraction s_anchor/L per layer, and how far the
    PHYSICAL anchor point teleports between adjacent layers (mm), to expose jumps."""
    fracs = []
    jump_mm = []
    prev_pt = None
    for li, loops in enumerate(layers):
        lp = max(loops, key=lambda l: l["L"])
        xy, s, L = lp["xy"], lp["s"], lp["L"]
        sa = cand._anchor_arc(xy, s, L)
        fracs.append(sa / L)
        # locate physical point at arc length sa
        j = int(np.argmin(np.mod(s - sa, L)))
        pt = xy[j]
        if prev_pt is not None:
            jump_mm.append(float(np.hypot(*(pt - prev_pt))))
        prev_pt = pt
    return np.array(fracs), np.array(jump_mm)


if __name__ == "__main__":
    attacks = {
        "twin_peak":   part_twin_peak,
        "flat_top":    part_flat_top,
        "near_circle": part_near_circle,
    }
    print(f"ANCHOR_DIR angle = {math.degrees(ANG):.2f} deg, lambda={LAMBDA}, amp={AMP}\n")
    print(f"{'part':12s} {'ourIL':>7s} {'propIL':>7s} {'il@evt':>7s} {'lam':>6s} "
          f"{'lamclm':>7s} {'amp':>6s} {'clo':>6s} {'isl':>4s} {'maxJmp':>7s} {'medJmp':>7s}")
    print("-" * 88)
    for name, fac in attacks.items():
        layers = fac()
        r = score_part(name, layers)
        fr, jmp = anchor_trace(layers)
        isl = "yes" if r["isl"] else "NO"
        ilev = r["il_ev"]
        ilev_s = "nan" if ilev != ilev else f"{ilev:.2f}"
        print(f"{name:12s} {r['il']:7.2f} {r['prop_il']:7.2f} {ilev_s:>7s} "
              f"{r['lam']:6.3f} {r['lam_climb']:7.3f} {r['amp']:6.3f} {r['clo']:6.3f} "
              f"{isl:>4s} {jmp.max():7.2f} {np.median(jmp):7.2f}")
    print("\n(maxJmp = largest layer-to-layer teleport of the physical anchor point, mm.")
    print(" A loop wall is ~100mm; a jump of tens of mm = the anchor hopped across the loop.)")
