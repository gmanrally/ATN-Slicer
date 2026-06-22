#!/usr/bin/env python3
"""From ATN_NODE logs, find which node field reliably marks the topmost roof node per XY column."""
import sys, re
from collections import Counter
rx = re.compile(r"ATN_NODE L=(\d+) srlb=(-?\d+) dtt=(-?\d+) dmt=([0-9.\-]+) px=([0-9.\-]+) py=([0-9.\-]+) kind=(\w+) ttl=(\d+)")
nodes = []
for line in open(sys.argv[1], errors="replace"):
    m = rx.search(line)
    if m:
        nodes.append(dict(L=int(m.group(1)), srlb=int(m.group(2)), dtt=int(m.group(3)),
                          px=float(m.group(5)), py=float(m.group(6)), ttl=int(m.group(8))))
if not nodes:
    print("no ATN_NODE lines"); sys.exit()
ttl = nodes[0]['ttl']
print(f"roof nodes: {len(nodes)}   top_interface_layers={ttl}")
print(f"srlb distribution: {dict(sorted(Counter(n['srlb'] for n in nodes).items()))}")
print(f"dtt  distribution: {dict(sorted(Counter(n['dtt'] for n in nodes).items()))}")

THR = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
clusters = []
for n in nodes:
    best = None; bd = THR*THR
    for cl in clusters:
        d = (n['px']-cl['px'])**2 + (n['py']-cl['py'])**2
        if d < bd: bd, best = d, cl
    if best is None: clusters.append(dict(px=n['px'], py=n['py'], ns=[n]))
    else:
        k=len(best['ns']); best['px']=(best['px']*k+n['px'])/(k+1); best['py']=(best['py']*k+n['py'])/(k+1); best['ns'].append(n)
print(f"XY columns (THR={THR}): {len(clusters)}")

top_srlb, non_srlb, top_dtt, non_dtt = Counter(), Counter(), Counter(), Counter()
for cl in clusters:
    ns = sorted(cl['ns'], key=lambda n: n['L'])
    topL = ns[-1]['L']
    for n in ns:
        (top_srlb if n['L']==topL else non_srlb)[n['srlb']] += 1
        (top_dtt  if n['L']==topL else non_dtt )[n['dtt']]  += 1
print(f"\nTOPMOST-per-column srlb: {dict(sorted(top_srlb.items()))}")
print(f"non-topmost        srlb: {dict(sorted(non_srlb.items()))}")
print(f"TOPMOST-per-column dtt : {dict(sorted(top_dtt.items()))}")
print(f"non-topmost        dtt : {dict(sorted(non_dtt.items()))}")
def score(name, topc, nonc, pred):
    tp=sum(v for k,v in topc.items() if pred(k)); tt=sum(topc.values())
    fp=sum(v for k,v in nonc.items() if pred(k)); tn=sum(nonc.values())
    print(f"  {name}: catches {100*tp/max(tt,1):.0f}% of topmost, fires on {100*fp/max(tn,1):.0f}% of non-topmost")
print("\ndiscriminator candidates:")
score(f"srlb=={ttl}",     top_srlb, non_srlb, lambda k:k==ttl)
score(f"srlb>={ttl}",     top_srlb, non_srlb, lambda k:k>=ttl)
score("dtt==0",           top_dtt,  non_dtt,  lambda k:k==0)
score("dtt<=0",           top_dtt,  non_dtt,  lambda k:k<=0)
