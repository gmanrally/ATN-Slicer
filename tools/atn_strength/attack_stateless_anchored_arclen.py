#!/usr/bin/env python3
"""Adversarial harness for candidate 'stateless-anchored-arclen'.

ATTACK ANGLE 2: topology change + many-layer drift.

Core finding: the candidate phases each loop as
    phase(point) = 2*pi*N*(s - s_anchor)/L,   N = round(L/lambda)   (integer, per loop)
with an integer-N hysteresis to resist boundary flips. The physical wavelength is
therefore L/N. For ADJACENT-LAYER registration to hold, layer N and layer N+1 must
realise the same number of waves at the same physical positions. But N is an INTEGER.
Any feature whose perimeter L drifts monotonically with height (every flared / tapered
/ domed / conical wall -- i.e. essentially every velocity-stack throat) MUST cross a
multiple-of-lambda boundary, at which N steps by 1. At that one interface the wave
count differs by a full wave distributed around the loop, so the two layers' Z fields
become UNCORRELATED -> a horizontal de-bond plane with ZERO Z interlock. Hysteresis
only DELAYS the crossing; it cannot remove it (a growing loop must eventually cross).

The shipped 'prop' engine does NOT have this failure: it carries the actual (cos,sin)
phase field up by interpolation, so it has no integer-N phase term to step.

PARTS (all physically real):
  * flared_cone  : a circular throat whose radius grows steadily over 120 layers
                   (a flared velocity-stack inlet). L sweeps ~6 lambda-boundaries.
  * fast_flare   : a steeper, shorter flare -> dense boundary crossings; the de-bond
                   planes are frequent enough that the LAYER-AVERAGED interlock itself
                   fails the gate.
  * split_pinch  : a near-circular feature that thins to a neck and SPLITS into two
                   near-equal children with centroids inside each other's match radius
                   (a bifurcating runner) -> exercises the hysteresis centroid mis-bind
                   at the split AND N-crossings on the growing parent.

Run:  python attack_stateless_anchored_arclen.py
"""
import math
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import validate_weave2 as J
import importlib.util

# load the candidate (filename has a hyphen, so import by path)
_spec = importlib.util.spec_from_file_location("cand", "cand_stateless-anchored-arclen.py")
cand = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cand)

RES = J.RES
LAMBDA = J.LAMBDA
AMP = J.AMP
SPEC = dict(field=cand.field)          # candidate as a harness method-spec
PROP = J._METHODS["prop"]


# ----------------------------------------------------------------- adversarial parts
def part_flared_cone(nlayers=120, seed=777):
    """A circular throat whose radius grows 18->22mm. Perimeter sweeps ~6 lambda
    boundaries -> ~6 N-flip de-bond interfaces. Physically real: a flared inlet."""
    rng = np.random.default_rng(seed)
    layers = []
    for k in range(nlayers):
        z = k / (nlayers - 1)
        r = 18.0 + 4.0 * z
        xy = J._ellipse(0, 0, r, r * 0.92, n=max(60, int(2 * math.pi * r / RES)))
        layers.append([J._loop(xy, 0, rng)])
    return layers


def part_fast_flare(nlayers=90, seed=900):
    """Steeper flare: circumference 14->38mm (L/lambda 3.5->9.5). Crossings are dense
    enough that the LAYER-AVERAGED interlock fails the gate. Physically real: a steep
    trumpet bell / boss flare."""
    rng = np.random.default_rng(seed)
    layers = []
    for k in range(nlayers):
        z = k / (nlayers - 1)
        circ = 14.0 + 24.0 * z
        r = circ / (2 * math.pi)
        xy = J._ellipse(0, 0, r, r * 0.9, n=max(48, int(circ / RES)))
        layers.append([J._loop(xy, 0, rng)])
    return layers


def part_split_pinch(nlayers=120, seed=778):
    """A body + a feature that grows as one near-circle, pinches to a neck, then SPLITS
    into two near-equal children whose centroids start inside each other's match radius
    (a bifurcating runner). Exercises both the hysteresis centroid mis-bind at the split
    and N-crossings on the growing parent."""
    rng = np.random.default_rng(seed)
    layers = []
    for k in range(nlayers):
        z = k / (nlayers - 1)
        loops = [("body", 0, J._ellipse(-30, 0, 12, 10, n=160))]
        if z < 0.50:
            r = 4.0 + 5.0 * z * 2.0  # parent grows -> crosses N boundaries before split
            loops.append(("feat", 1, J._ellipse(20, 0, r, r * 0.95,
                          n=max(60, int(2 * math.pi * r / RES)))))
        else:
            d = 2.2 + 12.0 * (z - 0.5) / 0.5
            loops.append(("featL", 1, J._ellipse(20 - d / 2, 0, 4.5, 4.3, n=120)))
            loops.append(("featR", 2, J._ellipse(20 + d / 2, 0, 4.5, 4.3, n=120)))
        if k % 2:
            loops = loops[::-1]
        layers.append([J._loop(xy, iid, rng) for (_, iid, xy) in loops])
    return layers


ADV_PARTS = {
    "flared_cone": part_flared_cone,
    "fast_flare":  part_fast_flare,
    "split_pinch": part_split_pinch,
}


# ----------------------------------------------------------------- analysis helpers
def n_flip_interfaces(layers, state):
    """Return [(layer_index, N_below, N_above, interlock_rms%, za/zb correlation)] for
    every interface where the candidate's stabilised integer N steps between adjacent
    layers on the LARGEST loop (the drifting feature)."""
    out = []
    for li in range(len(layers) - 1):
        a = max(layers[li], key=lambda l: l["L"])
        b = max(layers[li + 1], key=lambda l: l["L"])
        Na = cand._lookup_N(a, li, state)
        Nb = cand._lookup_N(b, li + 1, state)
        if Na == Nb:
            continue
        za = cand.field(a, li, "nested", state)
        zb = cand.field(b, li + 1, "nested", state)
        j, dist = J._match_pts(a["xy"], b["xy"], 0.5)
        ok = dist <= 0.5
        if not ok.any():
            continue
        e = za[ok] + zb[j[ok]]
        rms = 100.0 * np.sqrt(np.mean(e ** 2)) / AMP
        corr = float(np.corrcoef(za[ok], zb[j[ok]])[0, 1])
        out.append((li, Na, Nb, rms, corr))
    return out


def score(name, layers):
    state = cand.prepare(layers)
    pstate = PROP["prepare"](layers)
    ev = J.prop_event_counts(layers)
    il = J.interlock_rms_pct(layers, SPEC, state)
    ile = J.interlock_at_events(layers, SPEC, state, ev["event_layers"])
    pil = J.interlock_rms_pct(layers, PROP, pstate)
    lam = J.lambda_cov(layers, SPEC, state)
    b, t, climb = J.top_lambda_cov(layers, SPEC, state)
    amp = J.amp_cov(layers, SPEC, state)
    clo = J.closure_err(layers, SPEC, state)
    isl = J.per_island_ok(layers, SPEC, state)
    cov, mod = J.coverage_modulation(layers, SPEC, state)
    flips = n_flip_interfaces(layers, state)
    return dict(state=state, il=il, ile=ile, pil=pil, lam=lam, climb=climb,
                amp=amp, clo=clo, isl=isl, cov=cov, mod=mod, flips=flips,
                reseeds=ev["reseeds"], misbinds=ev["misbinds"])


# ----------------------------------------------------------------- gate (mirrors v2)
def gate_fail_reasons(r):
    reasons = []
    if not (r["il"] == r["il"] and r["il"] < 30.0):
        reasons.append(f"interlock={r['il']:.1f}>=30")
    has_events = (r["reseeds"] + r["misbinds"]) > 0
    if has_events and r["ile"] == r["ile"] and r["ile"] >= 35.0:
        reasons.append(f"il@event={r['ile']:.1f}>=35")
    if not (r["lam"] == r["lam"] and r["lam"] < 0.25):
        reasons.append(f"lam_cov={r['lam']:.2f}>=0.25")
    if not (r["amp"] == r["amp"] and r["amp"] < 0.20):
        reasons.append(f"amp_cov={r['amp']:.2f}>=0.20")
    if not (r["clo"] == r["clo"] and r["clo"] < 0.20):
        reasons.append(f"closure={r['clo']:.2f}>=0.20")
    if not r["isl"]:
        reasons.append("per_island_NO")
    return reasons


def main():
    print("=== ADVERSARIAL: stateless-anchored-arclen (Attack 2: topology + drift) ===")
    print(f"params lambda={LAMBDA} amp={AMP} res={RES}\n")
    results = {}
    for name, fac in ADV_PARTS.items():
        layers = fac()
        r = score(name, layers)
        results[name] = (layers, r)
        print(f"--- {name}  ({len(layers)} layers) ---")
        print(f"  ours interlock={r['il']:6.2f}%   prop interlock={r['pil']:6.2f}%   "
              f"(prop is the shipped engine: no integer-N phase term)")
        print(f"  il@event={r['ile']:.2f}  lam_cov={r['lam']:.3f}  amp_cov={r['amp']:.3f}  "
              f"closure={r['clo']:.3f}  per_island={'yes' if r['isl'] else 'NO'}  "
              f"cov={r['cov']:.2f} mod={r['mod']:.2f}")
        if r["flips"]:
            print(f"  N-FLIP DE-BOND INTERFACES ({len(r['flips'])}):")
            for li, Na, Nb, rms, corr in r["flips"]:
                print(f"    layer {li:3d}: N {Na}->{Nb}  interlock={rms:5.0f}%  "
                      f"za/zb corr={corr:+.2f}  "
                      f"{'<-- ZERO interlock (de-bond plane)' if abs(corr) < 0.25 else ''}")
        fails = gate_fail_reasons(r)
        print(f"  GATE: {'FAIL' if fails else 'pass'}"
              + ("   <- " + "; ".join(fails) if fails else ""))
        print()

    # viz
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    # (1) per-layer interlock for flared_cone, ours vs prop
    for ax, name in ((axes[0, 0], "flared_cone"), (axes[0, 1], "fast_flare")):
        layers, r = results[name]
        st = r["state"]; pst = PROP["prepare"](layers)
        ours, prop = [], []
        for li in range(len(layers) - 1):
            a = max(layers[li], key=lambda l: l["L"]); b = max(layers[li + 1], key=lambda l: l["L"])
            j, d = J._match_pts(a["xy"], b["xy"], 0.5); ok = d <= 0.5
            za = cand.field(a, li, "nested", st); zb = cand.field(b, li + 1, "nested", st)
            ours.append(100 * np.sqrt(np.mean((za[ok] + zb[j[ok]]) ** 2)) / AMP if ok.any() else np.nan)
            pa = PROP["field"](a, li, "nested", pst); pb = PROP["field"](b, li + 1, "nested", pst)
            prop.append(100 * np.sqrt(np.mean((pa[ok] + pb[j[ok]]) ** 2)) / AMP if ok.any() else np.nan)
        ax.plot(ours, "C3", lw=0.9, label="stateless-anchored-arclen")
        ax.plot(prop, "C0", lw=0.9, label="prop (shipped)")
        for li, *_ in r["flips"]:
            ax.axvline(li, color="C3", ls=":", lw=0.6)
        ax.axhline(30, color="k", ls="--", lw=0.6)
        ax.set_title(f"{name}: adjacent-layer interlock vs height", fontsize=10)
        ax.set_xlabel("layer"); ax.set_ylabel("interlock RMS (% amp, 0=ideal)")
        ax.legend(fontsize=8)
    # (3) the de-bond layer Z: za vs zb at matched arc position
    ax = axes[1, 0]
    layers, r = results["flared_cone"]; st = r["state"]
    li = r["flips"][1][0] if len(r["flips"]) > 1 else r["flips"][0][0]
    a = max(layers[li], key=lambda l: l["L"]); b = max(layers[li + 1], key=lambda l: l["L"])
    j, d = J._match_pts(a["xy"], b["xy"], 0.5); ok = d <= 0.5
    za = cand.field(a, li, "nested", st); zb = cand.field(b, li + 1, "nested", st)
    sL = a["s"][ok] / a["L"]; o = np.argsort(sL)
    ax.plot(sL[o], za[ok][o], "C0", lw=0.8, label=f"layer {li} Z")
    ax.plot(sL[o], zb[j[ok]][o], "C3", lw=0.8, label=f"layer {li+1} Z (matched)")
    ax.set_title(f"flared_cone N-flip @ layer {li}: walls drift OUT of antiphase", fontsize=10)
    ax.set_xlabel("arc position s/L"); ax.set_ylabel("Z (mm)"); ax.legend(fontsize=8)
    # (4) summary bars
    ax = axes[1, 1]
    names = list(ADV_PARTS.keys())
    x = np.arange(len(names))
    ax.bar(x - 0.2, [results[n][1]["il"] for n in names], 0.4, color="C3", label="ours")
    ax.bar(x + 0.2, [results[n][1]["pil"] for n in names], 0.4, color="C0", label="prop")
    ax.axhline(30, color="k", ls="--", lw=0.7)
    ax.set_xticks(x); ax.set_xticklabels(names, fontsize=8)
    ax.set_ylabel("avg interlock RMS (% amp)"); ax.set_title("Averaged interlock", fontsize=10)
    ax.legend(fontsize=8)
    fig.suptitle("Attack: integer-N phase steps at lambda-boundary crossings => "
                 "periodic Z de-bond planes on every drifting feature", fontsize=12)
    fig.tight_layout(rect=[0, 0, 1, 0.97])
    fig.savefig("attack_stateless_anchored_arclen.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print("wrote attack_stateless_anchored_arclen.png")


if __name__ == "__main__":
    main()
