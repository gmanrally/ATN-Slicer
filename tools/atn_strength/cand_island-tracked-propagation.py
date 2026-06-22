#!/usr/bin/env python3
"""Candidate method: ISLAND-TRACKED PROPAGATION (steelman of the shipped 'prop').

Registered into the FIXED judge (validate_weave2.py) as a method plugin and scored
through score_method WITHOUT touching the harness or its metrics.

------------------------------------------------------------------ design rationale

The shipped engine ('prop' in the harness) propagates the (cos,sin) phase FIELD itself
bottom-up by interpolated nearest-segment inheritance. Two things bite it:

  * lam_cov / lam_climb fanning: the inherited field is STRETCHED/COMPRESSED wherever the
    loop morphs, and that wavelength distortion ACCUMULATES up the stack (sharp_box 0.41,
    elongated 0.54, dome lam_climb 0.19 in the baseline run). Once the wavelength is
    non-uniform the weave bunches/fans -> requirements (2),(3),(4) fail.
  * mis-binds / re-seeds on the multi-island part: a Euclidean centroid match can bind a
    loop to the WRONG feature below, and a concave jump beyond the 2*lambda guard re-seeds
    the phase mid-loop -> a localised scramble.

STEELMAN (this candidate). Keep propagation for ADJACENT-LAYER REGISTRATION (req 1, the
headline) but FIX the three weaknesses the strategy calls out:

 (a) TRACK ISLANDS across layers, not just nearest centroid. Each persistent feature gets
     a stable track id from a centroid+area assignment that tolerates growth/drift and
     handles appear / disappear / merge. The track id (not a fragile Euclidean pick) is
     what we inherit phase along, so a body and a trumpet that pass close never swap.

 (b) MATCH ALONG THE LOOP (arc/geodesic nearest), not blind Euclidean across the gap. We
     only ever read the below-loop phase at the anchor's arc-nearest counterpart, and we
     reject a match whose nearest-point distance is large (a concave pocket) so we never
     inherit a phase from across a re-entrant gap.

 (c) DRIFT CORRECTION by RE-PROJECTING ONTO AN ARC-LENGTH FRAME EACH LAYER. Crucially we
     do NOT inherit the distorted field. On EVERY loop we REBUILD a perfectly uniform
     phase  phi(point) = 2*pi*N*(s - s0)/L  with N = round(L/lambda) an INTEGER (closure
     exact) and L/N the local wavelength (uniform by construction -> no fanning, no beat).
     The ONLY thing propagated from below is the scalar SEAM OFFSET s0 (one number per
     loop), chosen so this loop's phase at a tracked ANCHOR equals the below-loop's phase
     at the same physical point. So wavelength is re-derived clean every layer (kills
     fanning/drift) while the phase ORIGIN stays locked to the layer below (keeps the
     adjacent-layer interlock). Global registration (layer 0 vs 100) is deliberately NOT
     enforced -- only adjacency, which is all strength needs.

Closure: phi uses an INTEGER N, so phi(s=L) - phi(s=0) = 2*pi*N -> the (cos,sin) wave is
continuous across the seam by construction (closure_err ~ 0).

Honest weakness (noted at bottom): when N must CHANGE between two adjacent layers (L
crosses a round(L/lambda) boundary) one wave is added/removed and that single step cannot
be perfectly antiphase everywhere -- a localised, bounded interlock bump. We minimise it
by HYSTERESIS on N (only re-round when L drifts >0.5*lambda from the value that set the
current N) so N changes rarely and never chatters.
"""
import math
import numpy as np

import validate_weave2 as J   # the FIXED judge; we only register + score through it

LAMBDA = J.LAMBDA
AMP = J.AMP


# ----------------------------------------------------------------- small geometry
def _cum_s(xy):
    """Cumulative arc length (closed) for an Nx2 loop: s[i] = dist along to point i,
    starting at 0; also returns total perimeter L."""
    d = np.hypot(*(np.roll(xy, -1, axis=0) - xy).T)  # segment lengths, point i -> i+1
    s = np.concatenate([[0.0], np.cumsum(d)[:-1]])
    L = float(d.sum())
    return s, L


def _nearest_along(q, P, sP):
    """Arc/geodesic nearest point on closed polyline P (Mx2, arc-coords sP) for each
    query point q (Kx2). Returns the arc length s_hit at the nearest point (interpolated
    along the nearest segment, NOT snapped to a vertex) and the Euclidean distance.

    This is the 'match nearest ALONG the loop' step: we project onto the actual segments
    so a concavity is followed, and we hand back an ARC coordinate (so phase is read in
    the below-loop's own arc-length frame, the frame we propagate)."""
    A = P
    B = np.roll(P, -1, axis=0)
    AB = B - A
    AB2 = (AB ** 2).sum(1) + 1e-12
    segL = np.hypot(AB[:, 0], AB[:, 1])
    # param t of the projection of each q onto each segment
    t = ((q[:, 0][:, None] - A[:, 0]) * AB[:, 0] +
         (q[:, 1][:, None] - A[:, 1]) * AB[:, 1]) / AB2
    t = np.clip(t, 0.0, 1.0)
    cx = A[:, 0] + t * AB[:, 0]
    cy = A[:, 1] + t * AB[:, 1]
    d2 = (q[:, 0][:, None] - cx) ** 2 + (q[:, 1][:, None] - cy) ** 2
    j = np.argmin(d2, axis=1)
    ar = np.arange(len(q))
    s_hit = sP[j] + t[ar, j] * segL[j]
    dmin = np.sqrt(d2[ar, j])
    return s_hit, dmin


# ----------------------------------------------------------------- island tracking
def _track_islands(layers):
    """Assign every loop in every layer a STABLE track id following features across
    layers through appear / grow / drift / disappear / MERGE, using centroid + area.

    Greedy per layer: each current loop claims the nearest still-unclaimed previous-layer
    track whose centroid is within a radius scaled by the feature size (sqrt(area/pi) plus
    a slack for drift), preferring closer + similar-area candidates. Unclaimed current
    loops START A NEW TRACK (feature birth). Previous tracks left unclaimed simply end
    (death) or were absorbed (merge -> whichever current loop claims that centroid keeps
    its own track; the smaller absorbed one just dies). This is exactly the appear/merge
    handling the strategy asks for, done on geometry only (the engine has no island_id).

    Returns: track_of[li] = list (aligned with layers[li]) of integer track ids."""
    track_of = []
    prev = []          # list of (track_id, centroid, area) for the layer below
    next_track = 0
    for loops in layers:
        cur = []
        assigned = [None] * len(loops)
        used_prev = set()
        # candidate (cost, cur_idx, prev_idx) over all pairs within gate, then greedy
        cand = []
        for ci, lp in enumerate(loops):
            c = lp["centroid"]
            r = math.sqrt(max(lp["area"], 1.0) / math.pi)
            for pi, (tid, pc, pa) in enumerate(prev):
                dc = float(np.hypot(*(pc - c)))
                gate = r + math.sqrt(max(pa, 1.0) / math.pi) * 0.6 + 1.5  # size + drift slack
                if dc <= gate:
                    # cost rewards proximity AND area similarity (so a body doesn't steal a
                    # trumpet's track just by being nearer in absolute mm)
                    arat = abs(lp["area"] - pa) / max(lp["area"], pa, 1.0)
                    cand.append((dc / max(r, 1.0) + 1.2 * arat, ci, pi))
        cand.sort(key=lambda t: t[0])
        for _, ci, pi in cand:
            if assigned[ci] is not None or pi in used_prev:
                continue
            assigned[ci] = prev[pi][0]
            used_prev.add(pi)
        # births
        for ci in range(len(loops)):
            if assigned[ci] is None:
                assigned[ci] = next_track
                next_track += 1
        track_of.append(assigned)
        prev = [(assigned[ci], loops[ci]["centroid"], loops[ci]["area"])
                for ci in range(len(loops))]
    return track_of


# ----------------------------------------------------------------- the method
def _prepare(layers):
    """Bottom-up recurrence. For each loop build a UNIFORM-wavelength phase from its own
    arc length, with an integer wave count N (closure) and a seam offset s0 inherited from
    its tracked counterpart below (adjacent-layer registration). Store everything the
    field() needs keyed by (layer index, track id).

    Per loop we store: xy, s (arc), L, N, s0, and the resolved 'rec' so field() can look
    it up by track. We also store, for the layer above to read, the per-point phase as a
    function of arc length -> we simply re-evaluate phi from (s0, N, L)."""
    track_of = _track_islands(layers)

    # state[li] = dict track_id -> record
    state = []
    prev_recs = {}     # track_id -> record from layer below
    guard = 2.0 * LAMBDA

    for li, loops in enumerate(layers):
        cur = {}
        tids = track_of[li]
        for ci, lp in enumerate(loops):
            tid = tids[ci]
            xy = lp["xy"]
            s, L = _cum_s(xy)

            below = prev_recs.get(tid)   # ISLAND-TRACKED: only our own track's counterpart

            # ---- (c) re-derive a clean integer wave count -- HOLD N as long as possible ---
            # A CHANGE of N is a frequency mismatch between adjacent layers: 2*pi*N*u vs
            # 2*pi*(N+-1)*u drift to a full half-wave across the loop, so that one layer pair
            # cannot register no matter how s0 is chosen (a ~100% interlock layer). There is
            # NO seam offset that fixes a frequency mismatch -- this is the integer-closure
            # vs registration horn of the non-developable fact. So the right policy is to
            # change N AS RARELY AS POSSIBLE: hold below's N across a WIDE wavelength band and
            # only re-round when the implied wavelength leaves [0.62,1.6]*lambda (a 2.6x span).
            # On a collapsing dome this turns ~22 catastrophic single-steps into a handful of
            # unavoidable re-rounds. Honest cost: during a long hold the wavelength drifts away
            # from the lambda target (still perfectly UNIFORM along the loop, so lam_cov stays
            # ~0); we accept off-target wavelength to keep adjacent-layer registration, which
            # is the headline strength metric.
            n_raw = max(1, int(round(L / LAMBDA)))
            if below is not None:
                Nb = below["N"]
                lam_if_keep = L / max(Nb, 1)
                if 0.62 * LAMBDA <= lam_if_keep <= 1.60 * LAMBDA:
                    N = Nb                      # hold -> this layer pair registers exactly
                else:
                    N = n_raw                   # forced re-round (rare); this one layer pays
            else:
                N = n_raw
            N = max(1, N)

            # ---- (a)+(b) propagate the SEAM OFFSET s0 from the tracked loop below ------
            # WHOLE-LOOP least-squares alignment (not a single anchor): for EVERY point on
            # this loop, find its arc-nearest counterpart on the tracked loop below (geodesic
            # projection along the segments, so a concavity is followed not cut across) and
            # read the below-loop's uniform phase there. We then choose the one free scalar
            # -- the seam offset s0 -- as the value that best matches this loop's uniform
            # phase  2*pi*N*(s-s0)/L  to that inherited phase, in a CIRCULAR least-squares
            # sense. The optimum is closed-form: Delta = 2*pi*N*s0/L is the circular mean of
            # (this-phase-without-offset  -  inherited-phase). This spreads the unavoidable
            # morph error evenly around the loop (vs piling it at one anchor) and, for a pure
            # scaling like the dome with N unchanged, aligns the loops EXACTLY.
            #
            # Points whose nearest below-point is across a gap > guard (a re-entrant pocket)
            # are EXCLUDED from the fit -- we never inherit a phase from across a concave gap.
            # If too few points have a valid match (feature birth / fully new geometry) we
            # re-seed from a deterministic centroid-angle seam (clean uniform wave, not noise).
            reseed = False
            misbind = False
            if below is not None:
                s_hit, dmin = _nearest_along(xy, below["xy"], below["s"])
                valid = dmin <= guard
                if np.count_nonzero(valid) >= max(4, int(0.5 * len(xy))):
                    phi_below = 2.0 * math.pi * below["N"] * \
                        (np.mod(s_hit - below["s0"], below["L"])) / below["L"]
                    phi_here0 = 2.0 * math.pi * N * (np.mod(s, L)) / L  # with s0=0
                    # circular mean of (phi_here0 - phi_below) over valid points -> Delta
                    diff = phi_here0[valid] - phi_below[valid]
                    Delta = math.atan2(float(np.sin(diff).mean()), float(np.cos(diff).mean()))
                    # phi_here(s) = phi_here0 - Delta ;  Delta = 2*pi*N*s0/L
                    s0 = (Delta * L / (2.0 * math.pi * N)) % (L / N)
                    # if more than half the loop had no valid match, flag as a partial reseed
                    if np.count_nonzero(valid) < int(0.9 * len(xy)):
                        reseed = True
                else:
                    reseed = True
                    s0 = _seam_from_centroid(xy, s)
            else:
                reseed = True
                s0 = _seam_from_centroid(xy, s)

            rec = dict(xy=xy, s=s, L=L, N=int(N), s0=float(s0),
                       island_id=lp["island_id"], track=tid,
                       reseed=reseed, misbind=misbind, centroid=lp["centroid"])
            cur[tid] = rec
        state.append(cur)
        prev_recs = cur

    return dict(state=state, track_of=track_of)


def _seam_from_centroid(xy, s):
    """Deterministic seam: the arc position of the point whose centroid-angle is ~0. Used
    only on a fresh seed (feature birth or rejected concave match) so a re-seed is still a
    clean, repeatable, uniform wave -- not noise."""
    cx, cy = xy[:, 0].mean(), xy[:, 1].mean()
    k = int(np.argmin(np.abs(np.arctan2(xy[:, 1] - cy, xy[:, 0] - cx))))
    return float(s[k])


def _phi(rec, xy_query=None):
    """Uniform phase phi(point) = 2*pi*N*(s - s0)/L on the loop's OWN samples (xy_query is
    ignored here because the harness always evaluates field() on rec's own loop; we accept
    the loop dict's points and recompute s for robustness)."""
    s, L, N, s0 = rec["s"], rec["L"], rec["N"], rec["s0"]
    return 2.0 * math.pi * N * (np.mod(s - s0, L)) / L


def _lookup(loop, li, state_pack):
    """Resolve the loop the harness handed us to its stored record. The harness builds
    loop dicts fresh and may reorder islands, so we match by nearest centroid among THIS
    layer's records (each record carries its centroid). Track-id binding already happened
    in prepare; this just recovers which track this loop is."""
    cur = state_pack["state"][li]
    best = None
    bestd = 1e18
    for rec in cur.values():
        d = float(np.hypot(*(rec["centroid"] - loop["centroid"])))
        if d < bestd:
            bestd = d
            best = rec
    return best


def _field(loop, li, mode, state_pack):
    rec = _lookup(loop, li, state_pack)
    if rec is None:
        return np.zeros(len(loop["xy"]))
    xy = loop["xy"]
    s, L = _cum_s(xy)
    # Seam robustness: the harness may hand field() a loop whose seam (sample order) was
    # rolled relative to the array prepare() stored s0 against. We anchor the inherited s0
    # to a PHYSICAL point -- rec's first stored vertex -- by locating it on the loop we were
    # given (arc-nearest) and shifting s0 so the phase at that physical point is unchanged.
    # When prepare and field see the same array (the common case) s_anchor==0 and this is a
    # no-op; when the seam moved, the phase field is carried with the geometry, not the index.
    N = rec["N"]
    if rec["xy"] is xy:                       # identical array -> use stored s0 directly
        s0_here = rec["s0"]
    else:
        s_anchor, _ = _nearest_along(rec["xy"][0:1], xy, s)
        # phase rec assigns at its own xy[0] (s=0):  2*pi*N*(0 - s0)/L_rec
        phi_anchor = 2.0 * math.pi * N * (np.mod(-rec["s0"], rec["L"])) / rec["L"]
        s0_here = (s_anchor[0] - phi_anchor * L / (2.0 * math.pi * N)) % (L / N)
    phi = 2.0 * math.pi * N * (np.mod(s - s0_here, L)) / L
    return AMP * J._sign(li, mode) * np.sin(phi)


# register and (when run directly) score through the FIXED harness
J.register_method("island_prop", dict(prepare=_prepare, field=_field))


def main():
    # Build the parts EXACTLY as the harness does in its own main(), populate the engine
    # event cache it needs, then call the harness's own score_method on our method.
    print("Building parts via the fixed harness factories...")
    parts_cache = {}
    for pname, (fac, is_mi) in J.PARTS.items():
        layers = fac()
        parts_cache[pname] = (layers, is_mi)
        J._EVENTS[pname] = J.prop_event_counts(layers)
    twist_layers = J.TWIST_PART[1]()
    J._EVENTS["twist"] = J.prop_event_counts(twist_layers)

    res = J.score_method("island_prop", parts_cache)
    tw = J.score_twist("island_prop", twist_layers)

    parts = list(J.PARTS.keys())
    print("\n=== island_prop : ADJACENT-LAYER INTERLOCK RMS (% amp, 0=ideal) ===")
    for p in parts:
        print(f"  {p:13s} interlock={res[p]['interlock']:6.2f}  "
              f"il@evt={J._fmt(res[p]['il_event'],6,2).strip():>6s}  "
              f"lam_cov={res[p]['lam']:5.2f}  lam_climb={res[p]['lam_climb']:6.2f}  "
              f"amp_cov={res[p]['amp']:5.2f}  closure={res[p]['clo']:5.2f}  "
              f"island={'yes' if res[p]['isl'] else 'NO'}")
    g = res["_global"]
    print(f"\n  _global coverage={g['coverage']:.2f}  modulation={g['modulation']:.2f}")
    print(f"  twist interlock={tw:.2f}")

    # print machine-readable block the wrapper parses
    import json
    flat = {p: {k: (None if (isinstance(v, float) and v != v) else v)
                for k, v in res[p].items()} for p in parts}
    flat["_global"] = res["_global"]
    flat["twist"] = tw
    print("\nSCORES_JSON " + json.dumps(flat))


if __name__ == "__main__":
    main()
