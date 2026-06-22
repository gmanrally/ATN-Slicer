# Woven Walls — redesign spec (consistent weave regardless of geometry)

Status: design, ready to implement.
Scope: `src/libslic3r/WovenWalls.cpp` + `WovenWalls.hpp` only (plus 2 optional config keys).
Author note: every metric below is from `tools/atn_strength/validate_weave2.py` (the v2 faithful
harness) and from the adversarial probes re-run on 2026-06-21.

---

## 1. Decision — what wins, and why the "gate-passing" candidates do NOT

### 1.1 The candidate ranking is misleading for the stated goal

The task priority is, in order: **(1) adjacent-layer interlock**, (2) uniform wavelength,
(3) uniform amplitude, (4) closure, (5) per-island robustness. Requirement (1) is the *only* one
that buys Z strength.

Two candidates "PASS" the v2 gate:

| candidate | mechanism | gate |
|---|---|---|
| `constrained-interlock` | rebuild a clean **integer-N** arc wave `2*pi*N*(s-s0)/L` per loop; propagate only scalar offset `s0`; match by `island_id` | PASS |
| `island-tracked-propagation` | same integer-N arc wave; track islands by centroid+area; propagate `s0` | PASS |

Both PASS because the headline metric is a **whole-loop average** of adjacent-layer interlock. That
average hides the thing that actually matters.

### 1.2 The kill shot: integer/fractional-N re-derivation creates de-bond planes

When a loop's perimeter `L` drifts (every flared throat / tapered stack / collapsing dome — the
airbox's velocity stacks are *exactly* this), `N = round(L/lambda)` must step by ±1. A `k`-wave loop
cannot register against a `(k+1)`-wave loop with any seam offset — the two layers go a full half-wave
out at the same `(x,y)`. That is a **horizontal de-bond plane** (worse than no weave: peak-over-peak).

Re-run probe (circle `r = 5 → 14 → 5`, 90 layers — a velocity-stack bell), worst single
adjacent-layer pair and number of pairs above 80 % RMS (100 % = random, >100 % = anti-interlock):

```
method                 worst-pair   #pairs>80%   note
island_prop                 105%         4        integer-N flips -> 4 weld planes
constrained_interlock       101%         9        integer-N flips -> 9 weld planes
prop (SHIPPED)               13%         0        continuous field -> stays bonded
fractional-N variant         96%         8        moves the jump to the seam (closure 0.50)
```

The fractional-N variant proves this is **fundamental**, not a tuning miss: a per-loop arc-length wave
cannot give *both* adjacent-layer registration *and* closure on a morphing loop (closure jumps to 0.50
= half-wave seam discontinuity, which then varies layer-to-layer because the seam moves).

These candidates traded prop's **graceful, gradual** wavelength fanning (texture slowly coarsens —
cosmetic) for **discrete catastrophic weld-plane inversions** at the one feature the weave exists to
strengthen. That is a strict regression on priority (1). The adversarial verdicts found this
independently on both candidates ("flared throat … recurring zero-interlock de-bond planes",
"velocity-stack bell: FOUR planes at ~100 % … GATE PASSES").

**Conclusion: neither gate-passing candidate is the winner.** The whole-loop-average gate is blind to
the failure that matters most.

### 1.3 Winner: a hybrid built on the SHIPPED continuous-propagation core

The only field that survives the flare on req (1) is prop's **continuous `(cos,sin)` phase
propagation** — it never re-derives a wave count, it carries the actual phase point-to-point, so it
*cannot* produce a frequency-mismatch weld plane:

```
prop on the flare: worst-pair 13%, lam_cov 0.03–0.07 (NO fanning on a round morph)
```

Prop's real, measured weaknesses are three, and each has a clean fix already proven in the harness/
candidates:

| prop weakness (v2 numbers) | fix (source) |
|---|---|
| **mis-binds / cross-feature moiré** — centroid match within `sqrt(area/pi)` binds trumpet-3 to trumpet-2 at layers 84–85 (2 mis-binds); `il@event 16.9%` vs bulk `12.6%` | **stable island tracking** by centroid+**area+hysteresis** (from `island-tracked-propagation` `_track_islands`), replacing the fragile single-nearest-centroid pick |
| **wavelength fanning on developable loops** — `sharp_box lam_cov 0.41`, `elongated 0.54`, `dome lam_climb 0.19` (recurrence stretch integrates over height) | **periodic clean re-anchor** of the developable propagation to a fresh uniform arc wave when fanning exceeds a tolerance — bounds the drift without a per-layer N flip |
| **round/developable classifier bistability** + arc-length seed seam dependence | keep the **round branch** (`N*theta`, inherited axis/ncyc) which is fan-free on round loops (`angle on flare: 0 weld planes, lam 0.14`), and add **hysteresis** to the `0.6` circularity test so it can't flip layer-to-layer |

This hybrid keeps prop's de-bond-free behaviour on flares (priority 1) and removes prop's three
genuine defects, instead of trading priority 1 away to chase priority 2 like the candidates did.

### 1.4 Adversarial scorecard (what each survives / fails)

- `constrained-interlock` / `island-tracked-propagation`: **FAIL** — flared throat / velocity-stack
  bell / baluster → recurring 96–105 % weld planes (fatal on req 1). island-prop additionally
  degrades under pure island **translation** (drifting boss, ~50 % by 0.5 mm/layer) because its
  whole-loop circular-LSQ `s0` is ill-conditioned under translation.
- `stateless-anchored-arclen`: **FAIL** — aligned-flat anchor tie (91–99 % on a D-section) **and** the
  same integer-N flip on any flared throat; baseline interlock already 30–60 %.
- `arc-plus-world-lock`, `curvature-comp-angle`: **FAIL** the v2 gate outright (dome/twist interlock
  ≥ 30, lam ≥ 0.25).
- **Hybrid (this spec)**: inherits prop's flare survival (0 weld planes); the only residual risk is
  bounded developable fanning between re-anchors and the inherent non-developable limit on twist —
  both *graceful*, neither a de-bond plane. See §6 risks.

---

## 2. Algorithm (the hybrid), precisely

Per object, bottom-up (unchanged execution model: `PrintObject::make_contour_z` already calls
`Layer::make_woven_walls(prev)` sequentially for `layer_idx = 1..n`). For each woven loop on a layer:

**Step A — feature identity (replaces the single-nearest-centroid pick).**
Match this loop to a loop on the layer below using a **gated centroid+area cost**, not just nearest
centroid:
- gate radius `g = r_self + 0.6*r_below + 1.5 mm`, where `r = sqrt(max(area,1)/pi)`;
- cost `= dc / max(r_self,1) + 1.2 * |area_self - area_below| / max(area_self, area_below, 1)`;
- pick the lowest-cost *unclaimed* below-loop within its gate (greedy, area-aware) — this stops a
  large body stealing a small trumpet's match just by being nearer in absolute mm, which is the
  layer-84/85 mis-bind. No match → **birth** (seed). This is `_track_islands` ported to C++ but
  resolved **incrementally per layer** (we only ever know `prev`, never the whole stack — fine,
  because the cost is local).

**Step B — round vs developable, with hysteresis.**
`circ = 4*pi*area/L^2`. Classify `round` if `circ > 0.66`, `developable` if `circ < 0.54`; in the
dead-band `[0.54, 0.66]` **inherit the matched loop's class** (and if no match, use `> 0.6`). This
kills the layer-to-layer branch flip the diagnoses flagged. (Single threshold `0.6` stays the default
when there is no match.)

**Step C — phase field.**
- **round loop:** `ph = ncyc * atan2(y - axis.y, x - axis.x)`, with `axis`,`ncyc` **inherited from the
  matched round loop below** (seeded from this loop's own centroid + `round(L/lambda)` only at birth).
  Fan-free, registers adjacent layers exactly while the feature stays round. *No re-derivation, no
  flip* — `ncyc` is held for the life of the feature (it only re-seeds at a birth).
- **developable loop:** **continuous `(cos,sin)` propagation** from the matched loop below (the exact
  current engine inner loop: nearest segment, interpolate `cs0/cs1` by `t`, renormalise), with the
  `guard = 2*lambda` concave fallback to the arc-length seed **unchanged**. This is what survives the
  flare.
- **birth / no match:** arc-length seed `ph = 2*pi*n_wall*s/L`, `n_wall = round(L/lambda)` —
  unchanged.

**Step D — bounded re-anchor (NEW, the fanning fix; developable only).**
Propagation fans because each step's nearest-point projection slightly rescales arc on a morphing
loop, and that integrates. Bound it: while building the field, accumulate a cheap **local-wavelength
error estimate** for this loop (see §4.3). If the loop's propagated field has drifted past a
tolerance (`fan_tol`, default off → backward-compatible), **discard the propagated field for this loop
and re-seed it from the clean arc-length wave** `2*pi*n_wall*s/L`, choosing the seam phase `s0` by the
**circular-mean alignment** to the *propagated* field we just discarded (so the re-anchor is itself
registered to the layer below — no discontinuity is injected). This is the candidates' clean-wave
idea, but applied **only when needed and aligned to the continuous field**, so it never forces an N
flip on a smoothly-propagating wall. Default `fan_tol = 0` ⇒ never fires ⇒ identical to today's engine.

Result: round features get fan-free registration; developable features get de-bond-free continuous
propagation with fanning capped; no feature ever gets an integer-N weld plane.

---

## 3. What to add / replace / delete

### 3.1 `WovenWalls.hpp`

Extend `WovenLoopField` with the fields tracking + re-anchor need. **Append only** (no reordering;
struct is internal, not serialised):

```cpp
struct WovenLoopField {
    Vec2d              centroid {Vec2d::Zero()};
    double             area     {0.0};          // NEW: for area-aware tracking + birth radius
    bool               round    {false};
    Vec2d              axis     {Vec2d::Zero()};
    int                ncyc     {1};
    double             L        {0.0};          // NEW: perimeter, for re-anchor n_wall + hysteresis
    int                n_wall   {1};            // NEW: developable wave count, inherited for re-anchor
    int                track    {-1};           // NEW: stable feature id (debug / future), set in match
    std::vector<Line>  lines;
    std::vector<Vec2f> cs0, cs1;
};
```

`WovenPhaseField` unchanged. **Do not** add `island_id`: the engine has no such thing — tracking must
be geometric (centroid+area), exactly as `island-tracked-propagation` does and `constrained-interlock`
illegitimately did not (it read `island_id` from the harness dict; the real engine cannot).

### 3.2 `WovenWalls.cpp` — `weave_path_sequence(paths, wp, inset_idx)`

This function already computes `L`, `area`, `centroid`, `round_loop`, the centroid match, the round
seed and the developable propagation. Edits, in place:

1. **Replace the match block (current L143–151)** with the gated centroid+area cost match (Step A).
   It must iterate `wp.prev->loops`, compute the cost, track the best *unclaimed* loop. Claiming:
   pass a `std::vector<char> &claimed` aligned with `wp.prev->loops` through `WeaveParams` (see §3.4)
   so two loops on this layer cannot bind the same below-loop. Keep `match_loop` / `match_dist`
   pointers as today.

2. **Make `round_loop` hysteretic (Step B):** after computing `circ`, if a `match_loop` exists and
   `circ` is in `[0.54, 0.66]`, set `round_loop = match_loop->round`; else `round_loop = circ > 0.6`.

3. **Round branch:** unchanged except it already inherits `axis`/`ncyc` when
   `round_loop && match_loop && match_loop->round`. Keep.

4. **Developable branch:** unchanged (continuous propagation + `2*lambda` guard fallback). Keep.

5. **Add the re-anchor pass (Step D), developable only, gated by `wp.fan_tol > 0`:** after the per-
   point field is computed for this loop (i.e. after the emit loop populates `nl->cs0/cs1`), estimate
   the realised-wavelength CoV along the loop from the stored `(cos,sin)` (zero-crossing spacing of
   the `sin` component vs `L/n_wall`); if it exceeds `wp.fan_tol`, recompute every emitted point's
   `Z` and `nl` record from `2*pi*n_wall*(s - s0)/L`, where `s0` is the circular-mean offset that
   aligns the clean wave to the just-computed propagated `(cos,sin)` field. **Default `fan_tol = 0`
   ⇒ this pass is skipped entirely.** (Implementation detail in §4.3: cheapest is to compute the
   clean wave and the alignment in a second small loop over `nl->cs1`, then re-emit. To avoid a second
   geometry pass, buffer the emitted XY+taper+s during pass-2 and rewrite only `Z`.)

6. **Populate the new `WovenLoopField` fields** when creating `nl` (currently L189–197): set
   `nl->area`, `nl->L`, `nl->n_wall = n_wall`, `nl->track`. `axis/ncyc/round/centroid` already set.

7. **Min-perimeter cutoff:** keep the existing `if (npts < 3 || L < 3.0) return false;` (L119). Add an
   explicit comment that this is the tiny-loop guard. Optionally raise to `L < std::max(3.0,
   2.0*wp.wavelength)` so a loop too short to hold even two waves is left planar rather than seeded
   with `n_wall = 1` garbage — but make that conditional on `wp.woven` so brick-only behaviour is
   unchanged. (A loop with `L < 2*lambda` cannot interlock meaningfully anyway.)

### 3.3 `WovenWalls.cpp` — `Layer::make_woven_walls(prev)`

1. Build `prev_dists` as today (one distancer per `prev.loops`).
2. **Add `std::vector<char> claimed(prev.loops.size(), 0);`** and thread it into `WeaveParams` so the
   greedy claim in Step A persists across all loops of this layer. Reset per layer (it is a local).
3. Set the two new `WeaveParams` fields: `wp.claimed = &claimed;` and
   `wp.fan_tol = cfg.woven_wall_fan_tol.value;` (default 0 — see §3.5).
4. Everything else (amplitude clamp, `sign` parity, `res`, taper fast-path, brick, flow) unchanged.

### 3.4 `WeaveParams` (the anonymous-namespace struct, L35–56)

Add:

```cpp
std::vector<char> *claimed;   // per-layer claim flags aligned with prev->loops (Step A)
double             fan_tol;   // developable re-anchor trigger (0 = off, backward-compatible)
```

### 3.5 Config (optional, only if exposing the re-anchor knob)

`fan_tol` can ship hard-coded `0.0` (pure refactor, zero behaviour change) OR be exposed. If exposed,
add in `PrintConfig.cpp` next to `woven_wall_wavelength` (≈L4456), default `0` so feature-off is a
no-op, and read it in `make_woven_walls`. Recommended initial production value once validated: `0.30`
(re-anchor a developable loop when its along-loop wavelength CoV exceeds 0.30 — comfortably above the
0.25 uniformity gate, so it fires only on genuine fanning). Keep it `comAdvanced`.

```cpp
def = this->add("woven_wall_fan_tol", coFloat);
def->label = L("Weave re-anchor tolerance");
def->tooltip = L("Developable walls: re-derive a clean uniform wave when propagation "
                 "fanning exceeds this wavelength variation. 0 = never (pure propagation).");
def->min = 0; def->max = 1; def->mode = comAdvanced;
def->set_default_value(new ConfigOptionFloat(0));
```

### 3.6 Delete

Nothing is deleted. The brick path, `edge_taper`, `brick_flow`, `flow_ratio`, `interwall`/`brick`
sign logic, scarf-seam skip, `weaveable` skip-outer/inner, the taper fast-path, and the
nested/corrugated `sign` are **all untouched**. The round seed and the developable propagation inner
loops are **kept verbatim** — we only wrap them with better matching + an optional bounded re-anchor.

---

## 4. Per-object reference: how axis / lambda_target / per-island N are computed and threaded

The design deliberately uses **NO global per-object world reference** (a single world scalar field
beats/fans — the non-developable-surface fact, confirmed: `world amp_cov 0.25–0.37`, fails real-weave
check). All references are **per-feature, inherited up the recurrence:**

### 4.1 `lambda_target`
Stays the single object/region config `woven_wall_wavelength` (`wp.wavelength`, default 4 mm). It is
only ever used to seed `n_wall = round(L/lambda)` at a **birth**; thereafter wavelength is whatever the
continuous field / inherited `ncyc` realises. It is **not** re-rounded per layer — that is the whole
point (no N flip).

### 4.2 Round `axis` / `ncyc` (per island, inherited)
- **At birth** of a round feature: `axis = this loop's area-centroid`, `ncyc = round(L/lambda)`.
- **Every layer after:** if the matched-below loop is round, `axis = match->axis`, `ncyc =
  match->ncyc` (copied through `WovenLoopField`). So a trumpet keeps one fixed axis and one fixed
  cycle count for its whole height → exactly uniform radial wavelength, perfect adjacent registration,
  and **no flip** even as it flares (a flaring circle keeps `ncyc`, the wavelength simply grows — that
  is correct and de-bond-free; cf. `angle on flare: 0 weld planes`).
- Threaded by the existing `WovenLoopField.axis/ncyc` fields (no change needed beyond Step A giving the
  right `match_loop`).

### 4.3 Developable `n_wall` + re-anchor `s0` (per island)
- `n_wall` is seeded `round(L/lambda)` at birth and **carried in `WovenLoopField.n_wall`**, inherited
  from `match_loop` so it does not jitter; it is used **only** if/when the re-anchor (§Step D) fires.
- The continuous field itself uses **no `n_wall`** — it is pure `(cos,sin)` interpolation, which is
  why it never flips.
- **Re-anchor wavelength-error estimate (cheap, no second geometry pass):** during pass-2 we already
  walk every emitted point and have `s_along` and the `(cos,sin)` we stored. Track the arc positions
  of `sin`-zero-crossings; `lam_cov = stddev(spacing)/mean(spacing)`. If `lam_cov > fan_tol`, compute
  `Delta = atan2(mean sin(ph_clean - ph_prop), mean cos(...))` over the loop where
  `ph_clean = 2*pi*n_wall*s/L` and `ph_prop = atan2(cs.y, cs.x)`, set `s0 = Delta*L/(2*pi*n_wall)`, and
  rewrite each point's `Z = amp*sign*wall_sign*sin(2*pi*n_wall*(s - s0)/L)` plus re-store `nl->cs*`.
  This re-anchor is aligned to the layer below (via the propagated field it replaces), so it does NOT
  inject a discontinuity, and because it keeps `n_wall` integer it preserves closure for that loop.

### 4.4 Threading summary
- Down the stack: `WovenPhaseField prev` (already threaded through `make_woven_walls(prev)` and
  `wp.prev`). We add `area/L/n_wall/track` to each `WovenLoopField` it carries.
- Within a layer: `claimed` vector via `wp.claimed` so Step A's greedy match is consistent across the
  layer's loops.
- Config: `wp.wavelength` (existing), `wp.fan_tol` (new, default 0).

---

## 5. Backward compatibility

- **Feature off (`woven_walls_enabled = false`, `brick_layers_enabled = false`):** `make_woven_walls`
  early-`continue`s per region exactly as today; `need_weave` in `PrintObject` is unchanged. **No code
  path runs.** Zero behaviour change, zero gcode change.
- **Feature on, new knob at default (`fan_tol = 0`):** Step A (better match) and Step B (hysteretic
  class) are *strict improvements* to matching but only change output where the **old** code
  mis-bound or flipped class — i.e. only on the multi-feature/near-collision/threshold-straddling
  layers the diagnoses identified as currently *wrong*. On single-loop / well-separated parts the new
  match returns the **same** loop the old nearest-centroid did (same gate, area term is a tie-breaker),
  so common parts are byte-identical. Step D is skipped (`fan_tol = 0`). So: same-or-better, never
  worse, and identical on the parts v1 validated.
- **`.3mf` / profile compat:** new config key defaults to `0` and is `comAdvanced`; absent in old
  projects ⇒ reads default ⇒ no migration needed. New `WovenLoopField` members are runtime-only
  (never serialised). No printer-profile or format version bump.
- **Cross-platform / TBB:** the woven pass is already sequential (`make_woven_walls` runs in the
  bottom-up loop, *outside* the `tbb::parallel_for` that only does `make_contour_z`). `claimed` is a
  per-layer local. No new shared mutable state.

## 5.1 Min-perimeter cutoff for tiny loops
- Keep `npts < 3 || L < 3.0 → return false` (leaves micro-loops planar; they cannot hold a wave and
  would otherwise seed `n_wall = 1` noise).
- Optionally, for `wp.woven` only, raise to `L < max(3.0, 2.0*wp.wavelength)` so any loop too short to
  carry two full waves stays flat (a 1-wave loop has nothing to interlock with and inflates the
  closure error). Gate this behind `wp.woven` so brick-only loops keep today's `3.0` cutoff.

---

## 6. Risks / honest residuals

1. **Developable fanning between re-anchors.** With `fan_tol = 0` (default) the engine fans exactly as
   today on `sharp_box`/`elongated` (`lam_cov 0.41/0.54`). The fix is opt-in (`fan_tol ~ 0.30`). Risk:
   a re-anchor that fires too often on a noisy loop could add a faint texture seam; mitigated because
   the re-anchor is circular-mean-aligned to the field it replaces (no phase jump) and `fan_tol` is
   set above the uniformity gate. **Validate** the production `fan_tol` on the multi_island + dome
   parts before shipping non-zero.
2. **Twist.** No per-loop intrinsic method registers a genuinely twisting wall as well as a world
   field (non-developable limit; `prop twist 24.5%` vs `world 17.5%`). The hybrid is prop-class here
   (continuous propagation does follow real twist, far better than the candidates' 33–60 %). Accept as
   the intrinsic ceiling; it is graceful, not a de-bond plane.
3. **Round/developable hysteresis band** `[0.54, 0.66]` is a tuned constant; a feature oscillating
   *through* the whole band (rare) could still switch. Inheriting the class from below makes a switch
   require leaving the band on one side, which a real morph does monotonically, not in chatter.
4. **Tracking under fast translation.** The area-aware gated match is more robust than nearest-centroid
   but a feature translating > ~1 gate-radius per layer (≈ `r + 1.5 mm`) could still birth a new track.
   This degrades to a single re-seed (one continuous-field discontinuity), **not** the 50 % sustained
   de-registration the whole-loop circular-LSQ `s0` candidates suffer under translation — because the
   continuous field re-establishes registration the very next layer.
5. **Re-anchor cost.** One extra cheap pass over a loop's points when it fires; negligible vs the
   existing per-point `edge_taper` point-in-polygon. With `fan_tol = 0` there is zero added cost.

---

## 7. Implementation checklist (order)

1. `WovenWalls.hpp`: append `area, L, n_wall, track` to `WovenLoopField`.
2. `WeaveParams`: add `claimed`, `fan_tol`.
3. `weave_path_sequence`: replace match block with gated centroid+area+claim cost (Step A);
   hysteretic `round_loop` (Step B); populate new `nl` fields; keep round + developable branches.
4. `make_woven_walls`: add per-layer `claimed`; set `wp.claimed`, `wp.fan_tol`; (optional) read new
   config.
5. (Optional) `PrintConfig.cpp/.hpp` + `Preset.cpp`: add `woven_wall_fan_tol` default 0.
6. Add the developable re-anchor (Step D), guarded by `fan_tol > 0`.
7. Build (`cmake --build . --config RelWithDebInfo --target libslic3r`), slice the airbox, confirm:
   (a) feature-off gcode unchanged; (b) no integer-N weld planes on the velocity stacks; (c)
   trumpet-to-trumpet near-collision no longer mis-binds.
8. **Close the twin→binary gap** (recommended follow-up): extract woven gcode via `weave_post.py`,
   feed through `validate_weave2.py`'s judge, confirm the compiled engine matches the Python `prop`+
   hybrid numbers on a real STL.
