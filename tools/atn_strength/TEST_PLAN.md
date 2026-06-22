# Woven Walls — coupon test protocol (engine feature)

A rigorous, repeatable protocol to measure whether **woven walls** actually improve
Z-axis (interlayer) strength and watertightness, vs standard planar printing. The
goal is data that stands up: proper controls, enough replicates for statistics, and
every confounder pinned down. This supersedes the quick CLI matrix in `README.md`
(which used the post-processor); the feature now lives in the slicer engine.

> Why this matters: the literature supports *mechanical interlocking of layer
> interfaces* in general, but **no published study tests this specific method**
> (sinusoidal Z-modulated perimeters), and the dramatic interlock numbers people
> quote were over-stated / unverified. Your coupons are the new evidence.

---

## 1. Hypotheses (state them up front, test them)

- **H1 (strength):** Woven walls raise Z (interlayer) UTS vs planar control, at equal wall count.
- **H2 (mode):** Nested + inter-wall ("full basket") ≥ corrugated ≥ planar for Z UTS.
- **H3 (sealing):** Nested + inter-wall walls are watertight in PPA-CF where planar walls leak.
- **Null:** no significant difference vs control (what we must disprove with statistics).

Decide the **minimum effect size worth caring about** before testing (e.g. "+10% Z UTS
or it's not worth the print-time/visual cost").

---

## 2. Specimens

### 2.1 Z-tensile bar (primary)
- **Standard:** ISO 527-2 **1BA** (or ASTM D638 **Type IV**), printed **upright** so the
  **load axis = Z** and failure is across layer interfaces.
- **Walls-dominated:** set wall loops to **6**, **0–5% infill**, **0 top/bottom solid
  layers in the gauge** (or use a solid-wall cross-section) so the **woven walls carry
  the load**, not infill. The whole point is to load the feature under test.
- Print **flat (XY) copies** of the same bar as the **strength ceiling** reference.
- Practical note: tall thin uprights can wobble — add a small sacrificial brim/raft,
  keep the same for every specimen.

### 2.2 Watertightness specimen (for H3)
- A **capped thin-walled tube / cup**: ~Ø30 mm × 40 mm tall, **2–3 walls**, **0% infill**,
  a printed **bottom cap** (so the edge taper is exercised), open top.
- Same wall settings as the strength bars where possible.

---

## 3. Factors & levels (the matrix)

Hold everything constant except the woven settings. `woven_wall_edge_taper = 4` for all
(it only affects the top/bottom caps, not the tensile gauge). To sweep **nested**
amplitude you must also raise `woven_wall_max_sep` (nested clamps amplitude to
`max_sep/2`).

| Cell | enabled | nested | interwall | amplitude | wavelength | max_sep | Notes |
|---|---|---|---|---|---|---|---|
| **A** Control | off | – | – | – | – | – | planar baseline |
| **B** Corrugated | on | off | off | 0.40 | 4 mm | – | bold, no clamp |
| **C** Corrugated+IW | on | off | on | 0.15 | 4 mm | 0.30 | walls antiphase |
| **D** Nested | on | on | off | 0.15 | 4 mm | 0.30 | layers antiphase |
| **E** Full basket | on | on | on | 0.15 | 4 mm | 0.30 | layers+walls |
| **F** Basket, λ short | on | on | on | 0.15 | 2 mm | 0.30 | wavelength effect |
| **G** Basket, λ long | on | on | on | 0.15 | 8 mm | 0.30 | wavelength effect |
| **H** Basket, hi-amp | on | on | on | 0.25 | 4 mm | 0.50 | raised clamp |

Start with **A, B, D, E** (the core question); add **C, F, G, H** if the core shows signal.

---

## 4. Controls & fixed factors (pin these or the data is noise)

- **One machine, one nozzle, one spool/lot** per material. Record lot numbers.
- **Dry the nylon** (PA6, PPA-CF): record dryer temp/time and print within the dry window.
  Wet nylon ruins both strength and watertightness — this is the #1 confounder.
- **Identical profile** across cells except the woven settings: same temps, speeds,
  layer height, wall count, flow, **chamber temperature**, cooling.
- **Randomise print order** across cells (don't print all controls first) to spread
  machine drift / ambient changes.
- **Same orientation & position** habit; if printing several per plate, rotate which
  cell sits where between plates.
- **Weigh every specimen** — the amplitude clamp and Z-ripple slightly change deposited
  volume; mass flags over/under-extrusion that could confound strength.

---

## 5. Replication & sample size

- **n ≥ 5 per cell** (7 is better for nylon's variance). More replicates beat more cells.
- If a specimen has an obvious print defect (blob, layer shift, gap), **discard and
  reprint** — note it; don't test known-bad parts.
- Pilot the whole flow in **PLA first** (cheap, low variance) to shake out the rig and
  find promising settings, **then** run the real test in **PA6 and PPA-CF**.

---

## 6. Measurements

Per specimen:
- **Peak load (N)** and **UTS (MPa)** = peak load / measured cross-section (measure the
  actual printed cross-section with calipers — don't assume nominal).
- **Apparent modulus** (slope of the linear region) if the tester logs displacement.
- **Mass (g)** and **print time**.
- **Fracture surface photo** — woven interlock should show a different failure mode
  (tortuous / interlocked) vs the clean flat delamination of planar.
- Tester: a load cell / UTM if available; otherwise a documented **hanging-weight or
  lever rig** with a calibrated reference — record exactly what was used.

---

## 7. Watertightness protocol (H3)

Pick one and keep it identical across specimens:
- **Hydrostatic head:** fill the cup with **dyed water**, stand on dry paper towel,
  record **time to first seep** (dye on towel) up to a cutoff (e.g. 60 min). Pass = no
  seep at cutoff.
- **or Low-pressure bubble test:** seal, submerge, apply ~0.2–0.5 bar air, watch for
  bubble streams; record pass/fail and leak location.
- Test **control (A)** vs **full basket (E)** in **PPA-CF**, n ≥ 5 each. Report
  pass-rate and time-to-seep.

---

## 8. Analysis

- Report **mean ± SD** (and n) per cell; plot as a bar chart with error bars.
- **Significance:** Welch's t-test for each woven cell vs control A; if comparing many
  cells, one-way **ANOVA + Tukey HSD**. Report **p-values and effect size** (% change),
  not just "looks higher."
- Normalise Z UTS against the **XY ceiling** to report "% of XY strength recovered."
- Watertightness: report pass-rate (e.g. 5/5 vs 1/5) and median time-to-seep.

---

## 9. Recording template (one row per specimen)

```
material, lot, dried(Y/N,temp,hrs), cell, replicate#, plate#, position,
mass_g, cross_section_mm2, peak_load_N, UTS_MPa, modulus_MPa,
print_time_min, defect_notes, fracture_photo_id
```
Watertightness: `material, cell, replicate#, method, result(pass/fail), time_to_seep_min, leak_location`

---

## 10. What "good data" looks like

- A clear, statistically-significant Z-UTS difference (or a clear null) between control
  and the basket weave, with effect size, in both PLA and the engineering materials.
- A watertightness pass-rate that separates control from basket in PPA-CF.
- Fracture photos that physically explain the result.

That package — control vs woven, replicated, with stats and failure analysis — is what
turns "interesting anecdote" into a result worth publishing or building a feature on.
```
