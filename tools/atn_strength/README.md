# ATN strength toolpaths — woven-wall experiment

A post-processor + test plan for attacking FDM's weak Z-adhesion axis **without
multi-axis hardware**. The nozzle stays vertical; we modulate wall Z by a small
sub-layer amount so beads nest/interlock in XY *and* Z.

## Why this should work

A normal wall is a stack of flat rings welded only across a thin re-melt zone, so
Z strength is ~50–80% of XY. `weave_post.py` adds a sinusoidal Z ripple to each
perimeter. Every wall line in a layer shares the **same wave**; the choice of how
layers relate is the `--mode`:

- **nested** (default): each layer is the exact **antiphase** of the layer below,
  so a peak nests over a trough — an egg-carton / basket interlock that keys the
  layers against Z-tension.
- **corrugated**: every layer rides the *same* wave (no flip), so the inter-layer
  gap is constant and **never voids**, gaining bonded surface area + shear-key.

**The void trap (and the guard).** In nested mode the inter-layer gap swings by
`2 × amplitude` about the layer height. Push amplitude too far and the
trough-vs-peak side pulls apart into lens-shaped voids. So nested mode **clamps
amplitude** to keep the worst-case separation within `--max-sep` of a layer height
(default 0.3 → amp ≤ 0.15). For a bigger, more visible weave without voids, use
`--mode corrugated`, which has no such limit (only the collision cap).

**Registration (why corrugated could still void).** If the wave is measured as
arc length from the *print seam*, a moving seam or tiny per-layer loop-length
drift walks the wave out of phase between layers — every so often a whole layer
lands antiphase to its neighbour and you get a periodic horizontal **void band**,
even in corrugated mode. `--register geometry` (default) fixes this: it anchors
the wave to a fixed direction from each loop's centroid and uses a whole number
of cycles, so the *same physical point gets the same phase on every layer*
regardless of the seam. Use `--register seam` only to reproduce the old behaviour.
(Geometry registration assumes the loop shape is consistent layer-to-layer, which
holds for prisms/tubes; open paths fall back to the seam method automatically.)

It is a post-processor on purpose: cheap to iterate and measure before committing
to real ATN Slicer engine work.

## Required OrcaSlicer settings (before slicing the coupons)

| Setting | Value | Why |
|---|---|---|
| Use relative E distances | **ON** (M83) | clean per-segment extrusion split (M82 also handled) |
| Arc fitting | **OFF** | walls stay straight `G1`; `G2/G3` are passed through unmodified |
| Feature/verbose comments | **ON** (default) | needs `;TYPE:Outer wall` / `;TYPE:Inner wall` markers |

## Usage

Manual (writes a separate output file, or edits in place if you omit the output):

```bash
python weave_post.py in.gcode out.gcode --amp 0.35 --wavelength 4.0
python weave_post.py in.gcode            --amp 0.35   # in-place
```

### As an OrcaSlicer post-processing script (runs automatically on export)

OrcaSlicer's post-processing runner requires the **first token of the field to be
a real file on disk**, and it can only launch `.pl` and `.bat` directly — a bare
`python` triggers *"The configured post-processing script does not exist: python"*.
So point the field at the bundled **`weave.bat`** wrapper (full path, quoted):

`Print Settings -> Others -> Post-processing Scripts`:
```
"C:\Users\Graham Work\OrcaSlicer\tools\atn_strength\weave.bat" --amp 0.3 --wavelength 4
```

OrcaSlicer appends the gcode path; `weave.bat` forwards everything to the script,
which edits the gcode in place. After **Export G-code**, use **Reload from disk**
in the Preview (or open the file in the G-code viewer) to see the weave.
`weave.bat` uses the Windows `py` launcher — edit it if you want a specific Python.

| Flag | Default | Meaning |
|---|---|---|
| `--amp` | 0.35 | Z amplitude as a fraction of layer height |
| `--wavelength` | 4.0 | ripple wavelength along the path (mm) |
| `--min-seg` | 0.4 | path subdivision step (mm); smaller = smoother |
| `--taper` | 1.5 | seam taper (mm) so Z returns to base at the loop start/end |
| `--mode` | nested | `nested` = antiphase per layer (interlock); `corrugated` = same wave every layer (no voids) |
| `--max-sep` | 0.3 | nested only: max inter-layer separation (fraction of layer h); amplitude clamps to this |
| `--register` | geometry | `geometry` = anchor wave to part geometry (stops void bands); `seam` = legacy arc-length from seam |
| `--loop-phase` | 0 | optional phase offset between concentric loops in a layer (0 = all share one wave) |
| `--max-z-frac` | 0.5 | hard clamp on \|Z offset\| as a fraction of layer height (collision safety) |
| `--skip-first` | 1 | leave the first N layers planar (bed adhesion) |
| `--walls` | outer,inner | which wall types to weave |
| `--min-loop` | 3.0 | skip loops shorter than this (mm) |

The run prints a summary (`woven_loops`, `sub_segments`, `arcs_passed`). If
`woven_loops=0`, the `;TYPE:` markers are missing or `--walls` doesn't match.

## Safety / limits

- Amplitude is clamped to `max_z_frac × layer_height`, kept well under the
  vertical-nozzle collision limit. Start conservative (`--amp 0.3`) and inspect
  the first print for nozzle ticking.
- First layer is left planar by default.
- Flow is split by segment length; the extra path length from the small Z ripple
  is negligible at these amplitudes (slightly under-extruded peaks, by design).

## Coupon testing

The feature now lives in the slicer engine (Quality → Woven walls), so use the full
rigorous protocol in **[TEST_PLAN.md](TEST_PLAN.md)** — proper controls, replicates,
statistics, and a watertightness test. The quick matrix below is the original
post-processor pilot, kept for reference.

## Coupon test matrix (first round, post-processor pilot)

**Goal:** does woven-wall raise Z (interlayer) strength, and at what print-time cost?

- **Specimen:** rectangular tensile bar printed **upright (Z = load axis)** so the
  break is across layers. Print an **XY-oriented** copy of each as a control for
  the "ceiling." Walls-heavy (e.g. 4–6 perimeters, low infill) so the wall weave
  dominates the result.
- **Material:** PLA first (fast, cheap, low-variance) to find good parameters,
  then re-run the winner in **PA6** (the real target; ties into the warping work).
- **Replicates:** ≥5 per cell; report mean ± SD of break load / UTS.

| # | Condition | amp | wavelength | phase |
|---|---|---|---|---|
| A | Control (stock, no post) | — | — | — |
| B | Brick-bond seam only (slicer: staggered/random seam) | — | — | — |
| C | Weave, low amp | 0.20 | 4 mm | loop+layer |
| D | Weave, mid amp | 0.35 | 4 mm | loop+layer |
| E | Weave, high amp | 0.50 | 4 mm | loop+layer |
| F | Weave, long wavelength | 0.35 | 8 mm | loop+layer |
| G | Weave, short wavelength | 0.35 | 2 mm | loop+layer |
| H | Loop-phase only (no layer phase) | 0.35 | 4 mm | loop only |

**Record per cell:** Z break load, XY break load (control), print time, mass
(flags over/under-extrusion), and a photo of the fracture surface (woven vs flat).

**Decision:** if the best weave cell beats control Z-strength by a worthwhile
margin at acceptable time cost, promote it from post-processor to a real ATN
Slicer perimeter-generation option (and pair it with in-slicer brick-bond seam
staggering, which is a slicer setting rather than a post-process).

## Next steps if it pays off

1. Move the Z-modulation into perimeter generation (engine) so flow and seams are
   handled natively instead of post-hoc.
2. Add brick-bond bead offset per layer in the wall generator.
3. Expose `amp / wavelength / walls` as an ATN panel "Woven walls (beta)" toggle.
