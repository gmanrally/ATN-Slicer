#!/usr/bin/env python3
"""Verify tree-support top-contact selection from ATN_ROOFC debug log lines.

Each line: ATN_ROOFC L=<layer> z=<mm> contact=<0|1> exposed=<f> cx=<mm> cy=<mm>

Clusters roof groups into support columns by XY proximity, splits each into
contiguous-layer bands, and checks that EXACTLY the top (max-layer) group of
each band is contact=1 and all the rest are contact=0. That is the property
the fix must guarantee (fan only the single model-touching roof layer).

    python verify_roof_contact.py <slice_log.txt>
"""
import sys, re

import sys as _s
THR = float(_s.argv[2]) if len(_s.argv)>2 else 4.0

def main():
    rx = re.compile(r"ATN_ROOFC L=(\d+) z=([0-9.]+) contact=(\d) exposed=([0-9.\-]+) cx=([0-9.\-]+) cy=([0-9.\-]+)")
    pts = []
    for line in open(sys.argv[1], errors="replace"):
        m = rx.search(line)
        if m:
            pts.append(dict(L=int(m.group(1)), z=float(m.group(2)), c=int(m.group(3)),
                            cx=float(m.group(5)), cy=float(m.group(6))))
    if not pts:
        print("no ATN_ROOFC lines found (build w/ debug log + slice with --debug 2)"); return
    print(f"roof groups logged: {len(pts)}   contact=1: {sum(p['c'] for p in pts)} "
          f"({100*sum(p['c'] for p in pts)/len(pts):.0f}%)")

    # greedy XY clustering into columns
    clusters = []  # each: dict(cx,cy,pts)
    for p in pts:
        best = None; bd = THR*THR
        for cl in clusters:
            d = (p['cx']-cl['cx'])**2 + (p['cy']-cl['cy'])**2
            if d < bd: bd, best = d, cl
        if best is None:
            clusters.append(dict(cx=p['cx'], cy=p['cy'], pts=[p]))
        else:
            n=len(best['pts']); best['cx']=(best['cx']*n+p['cx'])/(n+1); best['cy']=(best['cy']*n+p['cy'])/(n+1)
            best['pts'].append(p)
    print(f"support columns (XY clusters): {len(clusters)}")

    bands=0; ok=0; bad=[]
    for cl in clusters:
        ps = sorted(cl['pts'], key=lambda p:p['L'])
        # split into contiguous-layer bands
        cur=[ps[0]]
        runs=[]
        for p in ps[1:]:
            if p['L'] <= cur[-1]['L']+1: cur.append(p)
            else: runs.append(cur); cur=[p]
        runs.append(cur)
        for band in runs:
            bands+=1
            topL=max(b['L'] for b in band)
            top_contacts=[b for b in band if b['L']==topL and b['c']==1]
            other_contacts=[b for b in band if b['L']<topL and b['c']==1]
            top_ok = len(top_contacts)>0
            rest_ok = len(other_contacts)==0
            if top_ok and rest_ok: ok+=1
            else:
                bad.append((cl['cx'],cl['cy'],len(band),topL,
                            "no contact at top" if not top_ok else f"{len(other_contacts)} contact(s) below top"))
    print(f"\nroof bands: {bands}")
    print(f"  CORRECT (top=contact, rest=non-contact): {ok} ({100*ok/max(bands,1):.0f}%)")
    print(f"  wrong: {bands-ok}")
    for cx,cy,n,tl,why in bad[:12]:
        print(f"    band @({cx:.0f},{cy:.0f}) len={n} topL={tl}: {why}")
    print("\nRESULT:", "PASS - top of every roof band is the contact layer"
          if bands and ok==bands else f"REVIEW - {bands-ok}/{bands} bands mis-tagged")

if __name__ == "__main__":
    main()
