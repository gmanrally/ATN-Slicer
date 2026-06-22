#!/usr/bin/env python3
"""SELF-CONTAINED adversarial attack on candidate 'island-tracked-propagation'.
(No dependency on shared scratch files that other agents may overwrite.)

Attack angle 2: topology change (split / merge / passer) + many-layer drift + N-rounding.
We build physically-real pathological parts, register the candidate via its own module,
and score it through the FIXED harness metrics. We also drill PER-LAYER / PER-ISLAND to
expose localized scrambles the whole-loop average hides.
"""
import math, os, sys, importlib.util
import numpy as np
import validate_weave2 as J

# import the TARGET candidate (registers 'island_prop' into J._METHODS)
_p = os.path.join(os.path.dirname(__file__), "cand_island-tracked-propagation.py")
_spec = importlib.util.spec_from_file_location("cand_island", _p)
cand = importlib.util.module_from_spec(_spec); sys.modules["cand_island"] = cand
_spec.loader.exec_module(cand)

RES, LAMBDA, AMP = J.RES, J.LAMBDA, J.AMP
_loop, _ellipse = J._loop, J._ellipse
SEED = 999
spec = J._METHODS["island_prop"]


# ---------------- geometry ----------------
def _peanut(cx, cy, sep, r, pinch, n=360, rot=0.0):
    th = np.linspace(0, 2*math.pi, n, endpoint=False)
    a = sep + r
    x = a*np.cos(th)
    hh = pinch + (r-pinch)*(np.abs(np.cos(th))**1.4)
    y = hh*np.sin(th)
    ct, st = math.cos(rot), math.sin(rot)
    return np.column_stack([cx + x*ct - y*st, cy + x*st + y*ct])


def part_merge_swap(nlayers=130):
    """Two equal islands merge (~L80); a small passer (id2) crosses between them L60-100."""
    rng = np.random.default_rng(SEED+4)
    layers = []; merge = 80
    for k in range(nlayers):
        z = k/nlayers; loops = []
        if k < merge:
            g = k/merge; sep = 14.0*(1-g)+3.5*g; r = 6.0
            loops.append(("L",0,_ellipse(-sep,0,r,r*0.9,n=int(2*math.pi*r/RES))))
            loops.append(("R",1,_ellipse(sep,0,r,r*0.9,n=int(2*math.pi*r/RES))))
        else:
            g = (k-merge)/max(1,nlayers-merge); w = 22.0+4.0*g; h = 12.0
            loops.append(("body",0,_ellipse(0,0,w/2,h/2,n=int(math.pi*(w+h)/2/RES))))
        if 60 <= k <= 100:
            pg = (k-60)/40.0; px = -10.0+20.0*pg
            loops.append(("passer",2,_ellipse(px,7.5,2.4,2.2,n=40)))
        if k % 2: loops = loops[::-1]
        layers.append([_loop(xy,iid,rng) for (_,iid,xy) in loops])
    return layers


def part_split(nlayers=130):
    """One peanut body whose waist thins and SPLITS (~L65) into two drifting disks."""
    rng = np.random.default_rng(SEED); layers = []; split = 65
    for k in range(nlayers):
        z = k/nlayers; loops = []
        if k < split:
            f = k/split; pinch = 6.0*(1-f)+0.25*f; sep = 9.0+4.0*z; r = 6.5
            loops.append(("body",0,_peanut(0,0,sep,r,pinch,n=360)))
        else:
            g = (k-split)/max(1,nlayers-split); sep = 13.0+8.0*g; r = 6.5+0.6*g
            loops.append(("left",0,_ellipse(-sep,0,r,r*0.92,n=max(48,int(2*math.pi*r/RES)))))
            loops.append(("right",1,_ellipse(sep,0,r,r*0.92,n=max(48,int(2*math.pi*r/RES)))))
        if k % 2: loops = loops[::-1]
        layers.append([_loop(xy,iid,rng) for (_,iid,xy) in loops])
    return layers


def part_n_chatter(nlayers=130):
    """Circle whose radius grows so L sweeps through many integer-N boundaries."""
    rng = np.random.default_rng(SEED+3); layers = []
    for k in range(nlayers):
        z = k/nlayers
        r = (10*LAMBDA/(2*math.pi)) + (10*LAMBDA/(2*math.pi))*z
        layers.append([_loop(_ellipse(0,0,r,r,n=max(64,int(2*math.pi*r/RES))),0,rng)])
    return layers


PARTS = {"merge_swap": part_merge_swap, "split": part_split, "n_chatter": part_n_chatter}


def per_layer(layers, state, eps=0.5):
    rows = []
    for li in range(len(layers)-1):
        Ai = J._island_loops(layers[li]); Bi = J._island_loops(layers[li+1])
        for iid, la in Ai.items():
            if iid not in Bi: continue
            lb = Bi[iid]
            za = spec["field"](la,li,"nested",state); zb = spec["field"](lb,li+1,"nested",state)
            j,d = J._match_pts(la["xy"],lb["xy"],eps); ok = d <= eps
            if not ok.any(): continue
            e = za[ok]+zb[j[ok]]
            rows.append((li,iid,100.0*math.sqrt(np.mean(e**2))/AMP,
                         100.0*np.max(np.abs(e))/AMP, int(ok.sum())))
    return rows


print(f"params lambda={LAMBDA} amp={AMP} res={RES}\n")
for pname, fac in PARTS.items():
    layers = fac()
    J._EVENTS[pname] = J.prop_event_counts(layers)
    state = spec["prepare"](layers)
    il = J.interlock_rms_pct(layers, spec, state)
    isl = J.per_island_ok(layers, spec, state)
    rows = per_layer(layers, state)
    bad = [r for r in rows if r[2] > 40]
    badpct = 100.0*len(bad)/max(1,len(rows))
    worst = sorted(rows, key=lambda r: r[2], reverse=True)[:5]
    # per-island pooled RMS (what the gate sees) + per-island MAX per-pair RMS (truth)
    pool = {}; permax = {}
    for (li,iid,rms,mx,n) in rows:
        pool.setdefault(iid,[]).append((rms,n)); permax[iid] = max(permax.get(iid,0), rms)
    print(f"=== {pname}: whole-loop interlock={il:.1f}%  per_island_ok={'yes' if isl else 'NO'}  "
          f"bad_pairs(RMS>40)={len(bad)}/{len(rows)} ({badpct:.0f}%) ===")
    for (li,iid,rms,mx,n) in worst:
        print(f"    worst: layer {li:3d} island {iid}  RMS {rms:5.1f}%  MAX {mx:5.1f}%  n={n}")
    for iid in sorted(permax):
        npairs = len(pool[iid]); tot_n = sum(n for _,n in pool[iid])
        print(f"    island {iid}: worst-pair RMS {permax[iid]:5.1f}%  over {npairs} pairs, {tot_n} pooled pts")
    print()
