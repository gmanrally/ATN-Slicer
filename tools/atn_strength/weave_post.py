#!/usr/bin/env python3
"""ATN woven-wall G-code post-processor (interlayer-strength experiment).

Adds a small, sub-layer-height sinusoidal Z modulation to perimeter (wall)
extrusions so that:
  - adjacent wall loops nest into each other (phase-shifted per loop), and
  - adjacent layers nest into each other (phase-shifted per layer),
turning the normally-flat stacked-ring interface into a woven/basket interface.
That multiplies the bonded interfacial area and mechanically keys material in
both XY and Z, attacking the weak Z-adhesion axis without any non-planar /
multi-axis hardware -- the nozzle stays vertical and the amplitude is kept well
under the collision limit.

It is deliberately a POST-PROCESSOR (not an engine change) so it is cheap to
iterate and measure before deciding on real ATN Slicer integration.

------------------------------------------------------------------------------
REQUIRED OrcaSlicer settings on the input .gcode (set before slicing):
  - "Use relative E distances" ON       -> emits M83 (also handles M82/absolute)
  - "Arc fitting" OFF                    -> walls are straight G1, not G2/G3
  - Verbose G-code / feature comments ON -> OrcaSlicer emits ;TYPE: lines (default)
G2/G3 arc moves inside walls are passed through unmodified (and counted).
------------------------------------------------------------------------------

Usage:
  python weave_post.py in.gcode out.gcode
      [--amp 0.35]          amplitude as a fraction of layer height
      [--wavelength 4.0]    sinusoid wavelength along the path, mm
      [--min-seg 0.4]       subdivision step, mm (smaller = smoother weave)
      [--taper 1.5]         seam taper length at each loop end, mm (Z->base)
      [--layer-phase 3.14159]  phase added per layer (pi = alternate layers)
      [--loop-phase 3.14159]   phase added per loop  (pi = alternate loops)
      [--max-z-frac 0.5]    hard clamp on |Z offset| as a fraction of layer h
      [--skip-first 1]      leave the first N layers planar (bed adhesion)
      [--walls outer,inner] which wall types to weave
      [--min-loop 3.0]      skip loops shorter than this (mm)
"""

import argparse
import math
import re
import sys

WALL_ALIASES = {
    "outer": "Outer wall",
    "inner": "Inner wall",
    "overhang": "Overhang wall",
}

_num = r"[-+]?\d*\.?\d+"
_axis_re = {a: re.compile(rf"(?:^|\s){a}({_num})") for a in "XYZEF"}


def _get(line, axis):
    m = _axis_re[axis].search(line)
    return float(m.group(1)) if m else None


def fmt(v, nd):
    return f"{v:.{nd}f}".rstrip("0").rstrip(".") if "." in f"{v:.{nd}f}" else f"{v:.{nd}f}"


class Weaver:
    def __init__(self, args):
        self.a = args
        self.walls = set()
        for w in args.walls.split(","):
            w = w.strip().lower()
            self.walls.add(WALL_ALIASES.get(w, w.title()))
        # machine / parsing state
        self.x = self.y = self.z = 0.0
        self.e_abs = 0.0           # running absolute E (for M82 input)
        self.rel_e = True          # default OrcaSlicer = relative E
        self.layer_z = 0.0         # true base Z of the current layer
        self.layer_h = args.fallback_lh
        self.layer_idx = 0
        self.cur_type = None
        self.loop_idx = 0          # loop counter within the current layer
        self.last_f = None         # last feedrate emitted/seen (raw or woven)
        # loop buffer: list of dicts {x0,y0,x1,y1,de,f}
        self.buf = []
        # stats
        self.n_loops = self.n_segs = self.n_arcs = 0

    # ---- helpers -----------------------------------------------------------
    def is_wall(self):
        return self.cur_type in self.walls and self.layer_idx > self.a.skip_first

    def base_phase(self):
        # Within a layer every wall line shares the same wave; only --loop-phase
        # (default 0) can offset adjacent concentric loops if you want to.
        return (self.loop_idx % 2) * self.a.loop_phase

    def layer_sign(self):
        # nested: each layer is the exact antiphase of the one below (sin -> -sin),
        # so a peak nests over a trough. corrugated: every layer rides the same
        # wave (no flip), so the inter-layer gap is constant (no voids).
        if self.a.mode == "corrugated":
            return 1.0
        return 1.0 if (self.layer_idx % 2 == 0) else -1.0

    # ---- loop flushing (the actual weave) ----------------------------------
    def flush(self, out):
        if not self.buf:
            return
        segs = self.buf
        self.buf = []
        L = sum(math.hypot(s["x1"] - s["x0"], s["y1"] - s["y0"]) for s in segs)
        # too short to weave meaningfully -> emit planar
        if L < self.a.min_loop:
            for s in segs:
                self._emit_move(out, s["x1"], s["y1"], self.layer_z, s["de"], s["f"])
            return

        self.n_loops += 1
        amp = self.a.amp * self.layer_h
        cap = self.a.max_z_frac * self.layer_h
        loop_phase = self.base_phase()
        sign = self.layer_sign()
        seg_lens = [math.hypot(s["x1"] - s["x0"], s["y1"] - s["y0"]) for s in segs]

        # Registration. "geometry" anchors the wave to a fixed direction from the
        # loop centroid and uses a whole number of cycles, so the SAME physical
        # point gets the SAME phase on every layer -- independent of the print
        # seam or small per-layer loop-length drift. This is what stops the
        # periodic void bands. Falls back to seam arc-length for open loops.
        reg = self._geometry_anchor(segs, seg_lens, L) if self.a.register == "geometry" else None
        taper = min(self.a.taper, L / 2.0)

        s_acc = 0.0
        for s, seg_len in zip(segs, seg_lens):
            if seg_len <= 1e-9:
                self._emit_move(out, s["x1"], s["y1"], self.layer_z, s["de"], s["f"])
                continue
            n = max(1, int(math.ceil(seg_len / self.a.min_seg)))
            for i in range(1, n + 1):
                frac = i / n
                x = s["x0"] + (s["x1"] - s["x0"]) * frac
                y = s["y0"] + (s["y1"] - s["y0"]) * frac
                s_here = s_acc + seg_len * frac
                if self.a.register == "world":
                    # World-space phase field: Z is a function of absolute (x,y), so the
                    # same physical point gets the same Z on every layer regardless of
                    # loop shape / seam / cross-section. Registers on any geometry.
                    kw = 2 * math.pi / self.a.wavelength
                    off = amp * sign * 0.5 * (math.sin(kw * x) + math.sin(kw * y))
                elif reg is not None:
                    s0, lam = reg
                    off = amp * sign * math.sin(2 * math.pi * ((s_here - s0) % L) / lam + loop_phase)
                else:
                    env = self._envelope(s_here, L, taper)
                    off = amp * env * sign * math.sin(2 * math.pi * s_here / self.a.wavelength + loop_phase)
                off = max(-cap, min(cap, off))
                # distribute this segment's extrusion evenly across its sub-steps
                self._emit_move(out, x, y, self.layer_z + off, s["de"] / n, s["f"])
                self.n_segs += 1
            s_acc += seg_len
        # each woven loop advances the phase so neighbouring loops can nest
        self.loop_idx += 1

    def _geometry_anchor(self, segs, seg_lens, L):
        """Return (s0, lam) so the wave is registered to part geometry, or None
        for open loops (caller falls back to seam arc-length + taper).

        s0 = arc length from the loop's print-start to the point nearest a fixed
        direction (+X) from the centroid. lam = L / round(L/wavelength) gives a
        whole number of cycles. Because the phase ends up a function of the
        *fractional* arc position (same physical point -> same fraction -> same
        phase), every layer registers vertically regardless of seam or length."""
        pts = [(segs[0]["x0"], segs[0]["y0"])] + [(s["x1"], s["y1"]) for s in segs]
        if math.hypot(pts[0][0] - pts[-1][0], pts[0][1] - pts[-1][1]) > 0.2:
            return None  # open loop -> not registerable this way
        # centroid over UNIQUE vertices (drop the duplicated closing point) so a
        # moving seam doesn't shift the centroid and break vertical registration
        uniq = pts[:-1]
        cx = sum(p[0] for p in uniq) / len(uniq)
        cy = sum(p[1] for p in uniq) / len(uniq)
        cum = [0.0]
        for sl in seg_lens:
            cum.append(cum[-1] + sl)
        best_i, best_d = 0, 9e9
        for i, (x, y) in enumerate(pts):
            d = abs(math.atan2(y - cy, x - cx))  # angular distance to +X (angle 0)
            if d < best_d:
                best_d, best_i = d, i
        n_cyc = max(1, round(L / self.a.wavelength))
        return cum[best_i], L / n_cyc

    @staticmethod
    def _envelope(s, L, taper):
        """Flat-top with cosine ramps so Z returns to base at both seam ends."""
        if taper <= 0:
            return 1.0
        if s < taper:
            return 0.5 * (1 - math.cos(math.pi * s / taper))
        if s > L - taper:
            return 0.5 * (1 - math.cos(math.pi * (L - s) / taper))
        return 1.0

    def _emit_move(self, out, x, y, z, de, f):
        parts = ["G1", "X" + fmt(x, 3), "Y" + fmt(y, 3), "Z" + fmt(z, 3)]
        if self.rel_e:
            parts.append("E" + fmt(de, 5))
        else:
            self.e_abs += de
            parts.append("E" + fmt(self.e_abs, 5))
        # only emit F when it actually changes (tracked across raw lines too)
        if f is not None and f != self.last_f:
            parts.append("F" + fmt(f, 0))
            self.last_f = f
        out.write(" ".join(parts) + "\n")
        self.x, self.y, self.z = x, y, z

    # ---- main line dispatch ------------------------------------------------
    def run(self, src, out):
        for line in src:
            raw = line.rstrip("\n")
            stripped = raw.strip()

            # E mode
            if stripped.startswith("M83"):
                self.rel_e = True
            elif stripped.startswith("M82"):
                self.rel_e = False

            # feature type
            if stripped.startswith(";TYPE:"):
                self.flush(out)
                self.cur_type = stripped[6:].strip()
                out.write(raw + "\n")
                continue

            # layer change
            if stripped.startswith(";LAYER_CHANGE"):
                self.flush(out)
                self.layer_idx += 1
                self.loop_idx = 0
                out.write(raw + "\n")
                continue
            if stripped.startswith(";HEIGHT:"):
                try:
                    self.layer_h = float(stripped[8:].strip())
                except ValueError:
                    pass
                out.write(raw + "\n")
                continue

            # comments / non-G1
            if not stripped or stripped.startswith(";"):
                out.write(raw + "\n")
                continue

            code = stripped.split()[0]

            # arcs inside walls: pass through, count, and flush any pending loop
            if code in ("G2", "G3"):
                if self.is_wall():
                    self.n_arcs += 1
                self.flush(out)
                self._track(raw)
                out.write(raw + "\n")
                continue

            if code in ("G0", "G1"):
                nx, ny, nz = _get(raw, "X"), _get(raw, "Y"), _get(raw, "Z")
                ne, nf = _get(raw, "E"), _get(raw, "F")
                x = nx if nx is not None else self.x
                y = ny if ny is not None else self.y
                de = self._delta_e(ne)
                extruding = (nx is not None or ny is not None) and de > 1e-9

                if self.is_wall() and extruding and code == "G1":
                    self.buf.append({"x0": self.x, "y0": self.y,
                                     "x1": x, "y1": y, "de": de, "f": nf})
                    # advance pos/E without emitting (emitted on flush)
                    self.x, self.y = x, y
                    if nz is not None:
                        self.layer_z = nz
                    continue
                else:
                    # any non-wall / travel / retract ends the current loop
                    self.flush(out)   # flush() advances loop_idx on woven loops
                    self._track(raw, nz_is_base=True)
                    out.write(raw + "\n")
                    continue

            # everything else
            self.flush(out)
            out.write(raw + "\n")

        self.flush(out)

    def _delta_e(self, ne):
        if ne is None:
            return 0.0
        if self.rel_e:
            return ne
        d = ne - self.e_abs
        return d

    def _track(self, raw, nz_is_base=False):
        nx, ny, nz = _get(raw, "X"), _get(raw, "Y"), _get(raw, "Z")
        ne, nf = _get(raw, "E"), _get(raw, "F")
        if nf is not None:
            self.last_f = nf       # keep F state in sync with passed-through lines
        if nx is not None:
            self.x = nx
        if ny is not None:
            self.y = ny
        if nz is not None:
            self.z = nz
            if nz_is_base:
                self.layer_z = nz
        if ne is not None:
            if self.rel_e:
                self.e_abs += ne
            else:
                self.e_abs = ne


def main(argv=None):
    p = argparse.ArgumentParser(description="ATN woven-wall G-code post-processor")
    p.add_argument("infile")
    # Optional. If omitted, edits infile IN PLACE -- this is the form OrcaSlicer's
    # "Post-processing Scripts" uses (it appends the temp gcode path as the last arg).
    p.add_argument("outfile", nargs="?", default=None)
    p.add_argument("--amp", type=float, default=0.35)
    p.add_argument("--wavelength", type=float, default=4.0)
    p.add_argument("--min-seg", dest="min_seg", type=float, default=0.4)
    p.add_argument("--taper", type=float, default=1.5)
    p.add_argument("--mode", choices=("nested", "corrugated"), default="nested",
                   help="nested = each layer is the antiphase of the one below "
                        "(egg-carton interlock); corrugated = every layer rides "
                        "the same wave (constant gap, never voids)")
    p.add_argument("--max-sep", dest="max_sep", type=float, default=0.3,
                   help="nested only: max allowed inter-layer separation as a "
                        "fraction of layer height; amplitude is clamped to keep "
                        "beads bonded (prevents the lens-shaped voids)")
    p.add_argument("--register", choices=("world", "geometry", "seam"), default="world",
                   help="world = world-space phase field F=(sin(kx)+sin(ky))/2; "
                        "registers across layers on ANY geometry incl. varying "
                        "cross-section (default). geometry = legacy per-loop arc "
                        "anchor (drifts on complex parts); seam = arc-length from seam")
    p.add_argument("--loop-phase", dest="loop_phase", type=float, default=0.0)
    p.add_argument("--max-z-frac", dest="max_z_frac", type=float, default=0.5)
    p.add_argument("--skip-first", dest="skip_first", type=int, default=1)
    p.add_argument("--walls", default="outer,inner")
    p.add_argument("--min-loop", dest="min_loop", type=float, default=3.0)
    p.add_argument("--fallback-lh", dest="fallback_lh", type=float, default=0.2)
    args = p.parse_args(argv)

    # Void guard: in nested (antiphase) mode the inter-layer gap swings by 2*amp
    # about the layer height. If that swing is too big the trough-vs-peak side
    # pulls apart into the lens-shaped voids. Clamp amplitude so the worst-case
    # separation stays within --max-sep of a layer height.
    if args.mode == "nested":
        max_amp = args.max_sep / 2.0
        if args.amp > max_amp:
            print(f"[weave] nested mode: amp {args.amp:.3f} would open voids up to "
                  f"{args.amp * 2:.2f}x layer height; clamping to {max_amp:.3f}. "
                  f"Use --mode corrugated for bigger amplitude without voids, or "
                  f"raise --max-sep to allow more separation.", file=sys.stderr)
            args.amp = max_amp

    in_place = args.outfile is None
    out_path = args.infile + ".weave.tmp" if in_place else args.outfile

    w = Weaver(args)
    with open(args.infile, "r", encoding="utf-8", errors="replace") as src, \
         open(out_path, "w", encoding="utf-8") as out:
        out.write(f"; ATN woven-wall post-process: amp={args.amp} lh-frac, "
                  f"wavelength={args.wavelength}mm, walls={args.walls}\n")
        w.run(src, out)

    if in_place:
        import os
        os.replace(out_path, args.infile)   # atomic in-place overwrite

    print(f"[weave] layers={w.layer_idx} woven_loops={w.n_loops} "
          f"sub_segments={w.n_segs} arcs_passed={w.n_arcs}", file=sys.stderr)
    if w.n_loops == 0:
        print("[weave] WARNING: no loops woven -- check that ;TYPE: comments are "
              "present and --walls matches (Outer/Inner wall).", file=sys.stderr)
    if w.n_arcs:
        print("[weave] NOTE: arc moves were passed through unmodified. Turn OFF "
              "'Arc fitting' in OrcaSlicer for full coverage.", file=sys.stderr)


if __name__ == "__main__":
    main()
