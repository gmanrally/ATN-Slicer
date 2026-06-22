# TengerTechnologies/Bricklayers — reference (for comparison only)

These are post-processing scripts from
https://github.com/TengerTechnologies/Bricklayers, vendored here purely to A/B
compare against ATN Slicer's native woven/brick features. **One local edit:**
`NonPlanarInterlockingWalls.py` `detect_slicer()` now also matches "ATN Slicer"
(an OrcaSlicer fork) so it uses the correct `;TYPE:Inner/Outer wall` markers
instead of silently falling back to PrusaSlicer markers and modulating nothing. **Licence: GPL-3.0**
(see `LICENSE`). Author: TengerTechnologies. Not part of the ATN Slicer build;
do not ship these in the installer.

- `bricklayers.py` — constant half-layer Z-stagger of *internal* perimeters
  (their keying is iteration-order `block % 2`; ATN's native brick keys on the
  stable shell `inset_idx`, so ATN is more robust on multi-island parts).
- `NonPlanarInterlockingWalls.py` — sine/triangle/etc. wall Z-modulation driven
  by a **world-space plane wave** `z += amp*wave(freq*(x|y|x+y))`. No per-feature
  or curved-surface handling (≈ ATN's early 2.5.1 world field).
- `vertical_bricklayers.py`, `bricklayersNonPlanarInfill.py` — other variants.

## How to run as an OrcaSlicer post-processor (Others → Post-processing Scripts)

First enable **Use relative E distances** (Filament → Advanced) — both scripts
scale `E` values, which is only correct with relative E.

Brick stagger (compare with ATN brick):
```
python "C:\Users\Graham Work\OrcaSlicer\tools\atn_strength\bricklayers_ref\bricklayers.py" -layerHeight 0.26 -extrusionMultiplier 1;
```

Non-planar wall weave (compare with ATN woven):
```
python "C:\Users\Graham Work\OrcaSlicer\tools\atn_strength\bricklayers_ref\NonPlanarInterlockingWalls.py" -include-perimeters -include-external-perimeters -wall-amplitude 0.3 -wall-frequency 1 -wall-direction xy -alternate-loops -perimeter-function sine;
```

Slice with ATN woven/brick **OFF** so the script is the only thing modulating,
then compare against a slice with ATN woven/brick **ON**.
