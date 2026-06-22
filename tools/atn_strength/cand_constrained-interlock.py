#!/usr/bin/env python3
"""Candidate woven-wall phase field: CONSTRAINED-INTERLOCK.

Registered to the FIXED judge harness (validate_weave2.py) via its method-plugin
contract:  register_method(name, {prepare(layers)->state, field(loop, li, mode, state)->Z}).
We do NOT modify the harness or its metrics; we add a method and score it.

-------------------------------------------------------------------------------
STRATEGY  (constrained-interlock)
-------------------------------------------------------------------------------
The non-developable-surface fact: no single WORLD scalar field gives BOTH uniform
wavelength on arbitrary loops AND vertical registration. So we split the two jobs:

  * WAVE SHAPE is defined WITHIN a layer purely by ARC LENGTH:
        phi_raw(s) = 2*pi * n_wall * s / L ,   n_wall = round(L / lambda).
    This is exactly uniform-wavelength (req 2), exactly uniform-amplitude (req 3),
    and exactly INTEGER-wave closed (req 4)  -- because the wave is a function of
    arc length with an integer number of cycles per perimeter.  None of these
    three properties depend on anything outside the loop, so they can never fan,
    beat, or open a seam.  That is the whole point of an intrinsic arc-length wave.

  * The ADJACENT-LAYER INTERLOCK (req 1, the headline) is then fixed WITHOUT
    touching the wave shape: we propagate ONLY a single scalar phase OFFSET (a
    rigid rotation of the otherwise-uniform arc wave) so that this loop's wave
    best lines up with the SAME-ISLAND loop on the layer BELOW, sampled at the
    nearest physical points.  We are NOT copying the layer-below wave SHAPE (that
    is what fans on a morphing section); we copy only the half-wave stagger
    RELATIONSHIP -- i.e. WHERE the peaks sit relative to the layer below.

    Concretely, given the uniform arc wave phi_raw on this loop, we choose the
    offset Delta that minimises the registration error to the layer-below stored
    phase phi_below sampled at each of this loop's points' nearest neighbour below.
    Because  sin / cos  are involved, the optimal rigid offset is closed-form:

        Delta* = atan2( sum sin(phi_below_k - phi_raw_k),
                        sum cos(phi_below_k - phi_raw_k) )

    (the circular-mean alignment).  We store  phi = phi_raw + Delta*  as the
    loop's field and as the (cos,sin) record propagated upward.  Adding a constant
    to phi_raw leaves wavelength, amplitude and closure EXACTLY unchanged (a closed
    integer-wave loop stays a closed integer-wave loop under a global phase shift),
    so requirements (2),(3),(4) are preserved by construction while (1) is optimised.

  * The harness applies the nested parity sign (-1)^li itself, so to realise the
    half-wave stagger (peak of N over trough of N+1) the FIELD on adjacent layers
    must carry the SAME phase at a shared physical point: then
        Z_N + Z_{N+1} = AMP*(+1)*sin(phi) + AMP*(-1)*sin(phi) = 0.
    Hence "best interlock" == "phi registers across adjacent layers", which is
    exactly what Delta* maximises.  We deliberately register ADJACENCY (N vs N+1)
    rather than the GLOBAL frame (layer 0 vs 100): the offset is propagated
    bottom-up so each step only has to agree with its immediate neighbour, which
    is all that interlayer strength needs.

  * PER-ISLAND independence + robustness:  matching is by island_id (the harness
    exposes it), so an airbox body never inherits stagger from a velocity-stack
    trumpet.  A loop with NO same-island counterpart below (feature birth) seeds
    Delta = 0 from a deterministic geometric seam (the +x centroid ray), so a new
    feature starts clean rather than scrambled.  n_wall can change as a feature
    grows; the offset is re-fit each layer so the stagger relationship is
    re-established every step rather than accumulating drift.

WEAKNESSES (honest, see report):
  * When n_wall changes between adjacent layers (a feature grows past a lambda
    boundary), a single rigid offset CANNOT make a k-wave loop register against a
    (k+1)-wave loop everywhere -- the wavenumbers differ, so some residual
    interlock error is unavoidable on the layers where round(L/lambda) ticks.
    Delta* still minimises it (circular best-fit), but it is not zero there.
  * Sharp corners / re-entrant pockets: the wave is a pure function of arc length,
    so it does NOT fan at corners (good), but the arc-length parameterisation
    redistributes slightly as the corner radius morphs, giving small registration
    residue near corners.
  * Registration is to the NEAREST physical neighbour below; across a large
    concave jump the nearest-neighbour pairing can be slightly oblique, adding a
    little residue (still bounded, since we only propagate a scalar).
"""
import math
import numpy as np

import validate_weave2 as J  # the FIXED judge (do not modify)

LAMBDA = J.LAMBDA
AMP = J.AMP

# n_wall hysteresis tolerance.  Adjacent layers register far better when they carry
# the SAME integer wave count, but L drifts continuously, so round(L/lambda) ticks
# often (e.g. a collapsing dome).  We INHERIT the layer-below n_wall as long as
# keeping it leaves the realised wavelength L/n_wall within HYST of lambda; only when
# the section has changed enough that this would violate uniform-wavelength do we
# re-snap to round(L/lambda).  The wave stays INTEGER-wave (closure preserved) and
# the realised wavelength stays within HYST of nominal (uniform-wavelength preserved),
# while wavenumber TICKS -- where a rigid offset cannot register k vs k+1 waves -- are
# made as rare as the uniform-wavelength budget allows.  0.18 < the 0.25 lam_cov gate.
HYST = 0.18


# --------------------------------------------------------------------- helpers
def _choose_n_wall(L, n_below):
    """Integer wave count for a loop of perimeter L.  Inherit n_below if doing so
    keeps the realised wavelength within HYST of lambda; else re-snap to round(L/lam).
    Always >= 1.  Integer => the arc wave closes with no seam jump (closure)."""
    nat = max(1, int(round(L / LAMBDA)))
    if n_below is None:
        return nat
    if abs((L / n_below) / LAMBDA - 1.0) <= HYST:
        return n_below
    return nat


def _uniform_arc_phase(loop, n_wall):
    """phi_raw(s) = 2*pi*n_wall*s/L on this loop.  n_wall is an INTEGER so the wave
    closes with a whole number of cycles (no seam jump).  Uniform in arc length ->
    uniform wavelength AND uniform amplitude by construction."""
    return 2.0 * math.pi * n_wall * loop["s"] / loop["L"]


def _geom_seam_offset(loop, phi_raw):
    """Deterministic seam anchor for a FEATURE-BIRTH loop (no counterpart below):
    put phase 0 at the point whose centroid-ray angle is nearest +x.  This is a
    pure function of the loop's own geometry, so a new feature is reproducible and
    not seam-jitter dependent (the harness rolls the seam to test exactly this).
    Returns the offset Delta0 such that phi = phi_raw + Delta0 has phase 0 there."""
    xy = loop["xy"]
    c = loop["centroid"]
    ang = np.arctan2(xy[:, 1] - c[1], xy[:, 0] - c[0])
    k = int(np.argmin(np.abs(np.mod(ang + math.pi, 2 * math.pi) - math.pi)))
    return -phi_raw[k]


def _nearest_idx(xy_q, xy_ref):
    """For each point in xy_q, index of nearest point in xy_ref + the distance."""
    d2 = ((xy_q[:, 0][:, None] - xy_ref[:, 0][None, :]) ** 2 +
          (xy_q[:, 1][:, None] - xy_ref[:, 1][None, :]) ** 2)
    j = np.argmin(d2, axis=1)
    return j, np.sqrt(d2[np.arange(len(xy_q)), j])


def _island_loops(layer):
    """Group a layer's loops by island_id (longest wins on the rare duplicate)."""
    out = {}
    for lp in layer:
        cur = out.get(lp["island_id"])
        if cur is None or lp["L"] > cur["L"]:
            out[lp["island_id"]] = lp
    return out


# ------------------------------------------------------------------- prepare()
def _prepare(layers):
    """Bottom-up recurrence.  For every loop store its uniform arc phase rotated by
    the rigid offset Delta* that best registers it to its SAME-ISLAND counterpart
    on the layer below (circular best-fit).  Propagate ONLY this scalar offset, not
    the wave shape.  state[li][island_id] -> dict(xy, phi).  We key by island_id so
    the field() lookup is O(1) and never cross-binds features."""
    states = []
    prev = None  # dict island_id -> dict(xy, phi) for the layer below
    for li, loops in enumerate(layers):
        by_isl = _island_loops(loops)
        cur = {}
        for iid, lp in by_isl.items():
            match = None if prev is None else prev.get(iid)
            n_below = None if match is None else match["n_wall"]
            n_wall = _choose_n_wall(lp["L"], n_below)
            phi_raw = _uniform_arc_phase(lp, n_wall)
            if match is None:
                # feature birth (or first layer): deterministic geometric seam.
                delta = _geom_seam_offset(lp, phi_raw)
            else:
                # propagate ONLY the stagger relationship: sample the layer-below
                # stored phase at this loop's nearest physical neighbours and pick
                # the single rigid offset that aligns the two waves best.
                j, dist = _nearest_idx(lp["xy"], match["xy"])
                phi_below = match["phi"][j]
                # circular mean of (phi_below - phi_raw): closed-form best rigid
                # rotation. Weight down points that pair across a big gap (>2*lambda)
                # so a re-entrant jump does not drag the whole offset.
                w = np.where(dist <= 2.0 * LAMBDA, 1.0, 0.05)
                d = phi_below - phi_raw
                delta = math.atan2(float(np.sum(w * np.sin(d))),
                                   float(np.sum(w * np.cos(d))))
            phi = phi_raw + delta
            cur[iid] = dict(xy=lp["xy"], phi=phi, n_wall=n_wall)
        states.append(cur)
        prev = cur
    return states


# --------------------------------------------------------------------- field()
def _field(loop, li, mode, state):
    """Return the Z modulation (mm) for this loop.  Look up the stored phase for
    this loop's island on this layer (computed in prepare()).  If the loop handed
    in is not the stored representative (rare duplicate / a non-representative of a
    merged island), recompute the uniform arc phase and align its rigid offset to
    the stored representative so the metric still sees a registered, uniform wave."""
    cur = state[li]
    rec = cur.get(loop["island_id"])
    if rec is None:
        # island not in stored layer (shouldn't happen): clean uniform arc seed.
        phi_raw = _uniform_arc_phase(loop, _choose_n_wall(loop["L"], None))
        phi = phi_raw + _geom_seam_offset(loop, phi_raw)
    elif rec["xy"] is loop["xy"] or (rec["xy"].shape == loop["xy"].shape and
                                     np.array_equal(rec["xy"], loop["xy"])):
        phi = rec["phi"]
    else:
        # same island, different sampling of the loop: reproject onto stored phase
        # using the SAME stored integer wave count (keeps closure + registration).
        phi_raw = _uniform_arc_phase(loop, rec["n_wall"])
        j, _ = _nearest_idx(loop["xy"], rec["xy"])
        d = rec["phi"][j] - phi_raw
        delta = math.atan2(float(np.sum(np.sin(d))), float(np.sum(np.cos(d))))
        phi = phi_raw + delta
    return AMP * J._sign(li, mode) * np.sin(phi)


# ------------------------------------------------------------------- register
J.register_method("constrained_interlock", dict(prepare=_prepare, field=_field))


# ----------------------------------------------------------------------- main
def _build_parts_and_events():
    """Reproduce what validate_weave2.main() builds: parts_cache + per-part engine
    event layers (needed by score_method for il_event / reseeds / misbinds).  We do
    NOT modify the harness; we call its own factories and event counter."""
    parts_cache = {}
    for pname, (fac, is_mi) in J.PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)
    twist_layers = J.TWIST_PART[1]()
    J._EVENTS["twist"] = J.prop_event_counts(twist_layers)
    return parts_cache, twist_layers


def main():
    parts_cache, twist_layers = _build_parts_and_events()
    name = "constrained_interlock"

    res = J.score_method(name, parts_cache)
    tw = J.score_twist(name, twist_layers)

    parts = list(J.PARTS.keys())
    print("=== candidate: constrained_interlock — REAL harness scores ===")
    print(f"params: lambda={LAMBDA}mm  amp={AMP}mm\n")

    print("--- adjacent-layer interlock RMS (% amp, 0=ideal, ~141=random) [HEADLINE] ---")
    for p in parts:
        print(f"  {p:13s} interlock={res[p]['interlock']:.2f}  "
              f"il@event={res[p]['il_event'] if res[p]['il_event']==res[p]['il_event'] else float('nan'):.2f}")
    print(f"  {'twist':13s} interlock={tw:.2f}  (scored separately)\n")

    print("--- full metrics per part ---")
    hdr = f"{'part':13s} {'interlk':>8s} {'il@evt':>7s} {'lam_cov':>8s} {'lamclmb':>8s} {'amp_cov':>8s} {'closure':>8s} {'island':>7s}"
    print(hdr)
    print("-" * len(hdr))
    for p in parts:
        r = res[p]
        print(f"{p:13s} {J._fmt(r['interlock'],8,2):>8s} {J._fmt(r['il_event'],7,2):>7s} "
              f"{J._fmt(r['lam'],8,3):>8s} {J._fmt(r['lam_climb'],8,3):>8s} "
              f"{J._fmt(r['amp'],8,3):>8s} {J._fmt(r['clo'],8,3):>8s} {J._fmt(r['isl'],7):>7s}")

    g = res["_global"]
    print(f"\n--- real-weave check (multi_island) ---")
    print(f"  coverage={g['coverage']:.3f}  modulation={g['modulation']:.3f}  "
          f"(need coverage>0.40 AND modulation>0.45)")

    # PASS-gate self-evaluation, replicating the harness gate thresholds exactly.
    print("\n--- PASS-gate self-check (harness thresholds) ---")
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
        reasons.append(f"twist.interlock={tw:.1f}>=30")
    if not (g["coverage"] > 0.40 and g["modulation"] > 0.45):
        reasons.append("not_a_real_weave")
    print("  PASS" if not reasons else "  FAIL <- " + "; ".join(reasons))

    # Emit a machine-readable summary line for the parent harness.
    import json
    summ = {p: {k: (None if isinstance(v, float) and v != v else v)
                for k, v in res[p].items()} for p in parts}
    summ["_global"] = res["_global"]
    summ["twist_interlock"] = tw
    summ["pass"] = not reasons
    summ["fail_reasons"] = reasons
    print("\nJSON " + json.dumps(summ))
    return res, tw, reasons


if __name__ == "__main__":
    main()
