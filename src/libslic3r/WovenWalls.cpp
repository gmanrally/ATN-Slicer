// ATN: Woven walls — sub-layer Z modulation of perimeters so layers interlock in XY+Z
// (inter-layer strength / sealing). Rides the existing z_contoured non-planar rails (see
// ContourZ.cpp + GCode::_extrude). Walls only; top/bottom solid surfaces stay planar.
//
// Registration is by PER-LOOP PHASE PROPAGATION: the woven step runs sequentially bottom-up
// (PrintObject contour_z), and each perimeter loop inherits its phase from ITS OWN
// counterpart on the layer below — matched by a GATED, AREA-AWARE, CLAIMED cost so two
// features can't bind the same counterpart and a large body can't steal a small trumpet's
// match just by being nearer in mm (that mis-bind was the moire on the velocity stacks). The
// round/developable classifier is hysteretic so it can't flip layer-to-layer near the
// threshold. The match is interpolated along the nearest segment and carried as a (cos,sin)
// unit vector. A loop with no counterpart below (first layer of a feature, or a re-entrant
// concave jump beyond the guard) is SEEDED by its shape: round loops (surfaces of
// revolution — trumpets) get a clean radial N*theta weave about their own axis; developable
// loops get a uniform arc-length weave. So vertical walls stay exact, the body follows its
// real outline, and each curved feature gets a deliberate radial texture instead of noise.
// Validated in tools/atn_strength/validate_weave.py.

#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "Point.hpp"
#include "AABBTreeLines.hpp"
#include "WovenWalls.hpp"
#include "libslic3r.h"

#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

struct WeaveParams {
    double       amp_mm;       // amplitude in mm (already clamped)
    double       wavelength;   // mm
    double       sign;         // +1 / -1 (layer parity for nested antiphase)
    double       res;          // subdivision resolution, mm
    const Layer *layer;        // for edge-taper neighbour lookups
    int          taper_k;      // fade the weave to flat over K layers at top/bottom (0 = off)
    bool         interwall;    // antiphase neighbouring shells so adjacent walls nest
    bool         woven;        // apply the sinusoid weave
    bool         brick;        // also apply a constant Z stagger on alternate shells
    double       brick_off_mm; // brick: Z shift applied to odd shells
    int          skip_outer;   // leave this many outermost walls planar
    int          skip_inner;   // leave this many innermost walls planar
    int          wall_count;   // nominal perimeters (wall_loops) for the inner count
    double       flow_ratio;   // extra extrusion multiplier on reshaped walls only
    double       brick_flow;   // extra extrusion multiplier on shifted brick shells
    // Phase propagation: the layer-below field + a distancer PER prev loop (aligned with
    // prev->loops; empty on the first woven layer -> seed), and this layer's accumulator.
    const WovenPhaseField                                  *prev;
    const std::vector<AABBTreeLines::LinesDistancer<Line>> *prev_dists;
    WovenPhaseField                                        *next;
    // Per-layer claim flags aligned with prev->loops: so two loops on this layer can't bind
    // the same below-loop (greedy area-aware feature tracking). nullptr on the first layer.
    std::vector<char>                                      *claimed;
};

// Total cross-section area (sum of lslices) of a layer, mm^2. Cheap, used to skip the
// expensive per-point taper test on interior layers.
double layer_lslices_area(const Layer *l)
{
    double a = 0.0;
    if (l != nullptr)
        for (const ExPolygon &ex : l->lslices)
            a += std::abs(ex.area());
    return a;
}

// Edge taper: 1.0 mid-wall, ramping to 0 as a wall reaches the top/bottom of the model.
double edge_taper(const Layer *layer, const Point &p, int K)
{
    if (K <= 0 || layer == nullptr)
        return 1.0;
    auto covered = [&p](const Layer *l) -> bool {
        if (l == nullptr)
            return false;
        for (const ExPolygon &ex : l->lslices)
            if (ex.contains(p))
                return true;
        return false;
    };
    int up = 0;
    for (const Layer *l = layer->upper_layer; l != nullptr && up < K; l = l->upper_layer) {
        if (covered(l)) ++up; else break;
    }
    int down = 0;
    for (const Layer *l = layer->lower_layer; l != nullptr && down < K; l = l->lower_layer) {
        if (covered(l)) ++down; else break;
    }
    const double t = double(std::min(up, down)) / double(K);
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

// Reshape one perimeter shell onto the weave and record its phase field for the layer
// above. inset_idx (stable shell index, 0 = outermost) drives brick + inter-wall antiphase.
bool weave_path_sequence(std::vector<ExtrusionPath> &paths, const WeaveParams &wp, int inset_idx)
{
    const int d = inset_idx >= 0 ? inset_idx : 0;

    // ---- pass 1: length + vertex count + area centroid (feature matching / round seed) ----
    double L = 0.0;
    int    npts = 0;
    Vec2d  prevpt(0, 0), firstpt(0, 0);
    bool   have_prev = false;
    double a2 = 0.0, gx = 0.0, gy = 0.0, vsx = 0.0, vsy = 0.0;
    for (const ExtrusionPath &p : paths)
        for (const Point3 &q : p.polyline.points) {
            const Vec2d cur(unscale_(q.x()), unscale_(q.y()));
            if (!have_prev) {
                firstpt = cur;
            } else {
                L += (cur - prevpt).norm();
                const double cr = prevpt.x() * cur.y() - cur.x() * prevpt.y();
                a2 += cr; gx += (prevpt.x() + cur.x()) * cr; gy += (prevpt.y() + cur.y()) * cr;
            }
            vsx += cur.x(); vsy += cur.y();
            prevpt = cur; have_prev = true; ++npts;
        }
    // Tiny-loop cutoff. A loop too short to carry two full waves has nothing to interlock
    // with and only adds closure noise, so leave it planar. Brick keeps the plain 3 mm floor.
    const double min_L = wp.woven ? std::max(3.0, 2.0 * wp.wavelength) : 3.0;
    if (npts < 3 || L < min_L)
        return false;
    {   // close the loop for the area centroid
        const double cr = prevpt.x() * firstpt.y() - firstpt.x() * prevpt.y();
        a2 += cr; gx += (prevpt.x() + firstpt.x()) * cr; gy += (prevpt.y() + firstpt.y()) * cr;
    }
    const Vec2d  centroid = std::abs(a2) > 1e-9 ? Vec2d(gx / (3.0 * a2), gy / (3.0 * a2))
                                                : Vec2d(vsx / npts, vsy / npts);
    const double area = std::abs(a2) * 0.5;
    // Circularity 4*pi*area/perimeter^2 ~ 1 for a circle (a surface of revolution -> seed a
    // clean radial weave about its own axis), lower for a developable wall (-> arc-length).
    // round_loop is classified AFTER the feature match below, with hysteresis, so a loop
    // straddling the threshold can't flip its whole phase basis layer-to-layer.
    const double circ = L > 1e-9 ? (4.0 * M_PI * area / (L * L)) : 0.0;

    // Brick: a constant Z stagger on odd shells, keyed on the stable shell index.
    const double brick_off = (wp.brick && (d & 1)) ? wp.brick_off_mm : 0.0;
    if (wp.brick && !wp.woven && brick_off == 0.0)
        return true; // brick-only even shell stays planar

    const double wall_sign = (wp.interwall && (d & 1)) ? -1.0 : 1.0;
    const int    n_wall    = std::max(1, int(std::lround(L / wp.wavelength)));
    const double guard     = 2.0 * wp.wavelength; // concave fallback distance (mm)

    // Step A — match this loop to ITS counterpart on the layer below by a GATED centroid+area
    // cost, CLAIMING it so two loops on this layer can't bind the same below-loop. Area-aware
    // so a large body can't steal a small trumpet's match just by being nearer in mm (that was
    // the velocity-stack mis-bind). No match within the gate -> birth (seeded by shape).
    const AABBTreeLines::LinesDistancer<Line> *match_dist = nullptr;
    const WovenLoopField                      *match_loop = nullptr;
    if (wp.woven && wp.prev != nullptr && wp.prev_dists != nullptr) {
        const double r_self    = std::sqrt(std::max(area, 1.0) / M_PI);
        double       best_cost = std::numeric_limits<double>::max();
        int          best_i    = -1;
        for (size_t i = 0; i < wp.prev->loops.size(); ++i) {
            if (wp.claimed != nullptr && i < wp.claimed->size() && (*wp.claimed)[i])
                continue; // already bound to another loop on this layer
            const WovenLoopField &pl      = wp.prev->loops[i];
            const double          r_below = std::sqrt(std::max(pl.area, 1.0) / M_PI);
            const double          dc      = (pl.centroid - centroid).norm();
            if (dc > r_self + 0.6 * r_below + 1.5)
                continue; // outside this feature's gate
            const double cost = dc / std::max(r_self, 1.0)
                              + 1.2 * std::abs(area - pl.area) / std::max(std::max(area, pl.area), 1.0);
            if (cost < best_cost) { best_cost = cost; best_i = int(i); }
        }
        if (best_i >= 0) {
            match_loop = &wp.prev->loops[best_i];
            match_dist = &(*wp.prev_dists)[best_i];
            if (wp.claimed != nullptr && size_t(best_i) < wp.claimed->size())
                (*wp.claimed)[best_i] = 1;
        }
    }

    // Step B — round vs developable, with hysteresis: in the dead-band inherit the matched
    // loop's class so the phase basis can't flip layer-to-layer near the threshold.
    const bool round_loop = (match_loop != nullptr && circ >= 0.54 && circ <= 0.66)
                                ? match_loop->round
                                : (circ > 0.6);

    // Round loops are surfaces of revolution: weave a DETERMINISTIC radial N*theta about a
    // FIXED axis + cycle count (inherited from below so they stay consistent up the whole
    // feature) -> no propagation accumulation, perfectly even radial pattern. Developable
    // loops inherit phase from the matched loop below (propagation), else arc-length seed.
    Vec2d axis_use = centroid;
    int   ncyc_use = n_wall;
    if (round_loop && match_loop != nullptr && match_loop->round) {
        axis_use = match_loop->axis;
        ncyc_use = match_loop->ncyc;
    }
    auto phase_cs = [&](double x, double y, double s_along) -> Vec2f {
        double ph;
        if (round_loop) {
            ph = double(ncyc_use) * std::atan2(y - axis_use.y(), x - axis_use.x());
        } else {
            if (match_dist != nullptr && match_loop != nullptr && !match_loop->lines.empty()) {
                auto [dist, idx, cp] = match_dist->distance_from_lines_extra<false>(Point::new_scale(x, y));
                if (unscale_(dist) <= guard && idx < match_loop->lines.size()) {
                    const Line  &ln = match_loop->lines[idx];
                    const Vec2d  a(double(ln.a.x()), double(ln.a.y()));
                    const Vec2d  ab(double(ln.b.x() - ln.a.x()), double(ln.b.y() - ln.a.y()));
                    const double ab2 = ab.squaredNorm();
                    double t = ab2 > 1e-9 ? (Vec2d(cp.x(), cp.y()) - a).dot(ab) / ab2 : 0.0;
                    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                    Vec2f       cs = match_loop->cs0[idx] * float(1.0 - t) + match_loop->cs1[idx] * float(t);
                    const float n  = std::sqrt(cs.x() * cs.x() + cs.y() * cs.y());
                    if (n > 1e-6f) cs /= n;
                    return cs;
                }
            }
            ph = 2.0 * M_PI * double(n_wall) * s_along / L; // arc-length seed / concave fallback
        }
        return Vec2f(float(std::cos(ph)), float(std::sin(ph)));
    };

    // New loop entry handed to the layer above (woven loops only).
    WovenLoopField *nl = nullptr;
    if (wp.woven && wp.next != nullptr) {
        wp.next->loops.emplace_back();
        nl = &wp.next->loops.back();
        nl->centroid = centroid;
        nl->area     = area;
        nl->round    = round_loop;
        nl->axis     = axis_use;
        nl->ncyc     = ncyc_use;
        nl->L        = L;
        nl->n_wall   = n_wall;
    }

    double s_run      = 0.0;
    Vec2d  prev2(0, 0);
    bool   have2      = false;
    double taper_prev = 1.0;
    for (ExtrusionPath &p : paths) {
        const Points3 pts = p.polyline.points; // copy; we overwrite p.polyline
        if (pts.empty())
            continue;
        Polyline3 np;

        // Record this loop's base phase into its new entry for the layer above.
        Point rec_pt;
        Vec2f rec_cs;
        bool  rec_have = false;
        auto  record   = [&](const Point &sp, const Vec2f &cs) {
            if (rec_have && nl != nullptr) {
                nl->lines.emplace_back(rec_pt, sp);
                nl->cs0.push_back(rec_cs);
                nl->cs1.push_back(cs);
            }
            rec_pt = sp; rec_cs = cs; rec_have = true;
        };
        auto emit = [&](double x, double y, double taper, double s_along) {
            const Vec2f  cs    = phase_cs(x, y, s_along);
            const double woven = wp.woven ? wp.amp_mm * wp.sign * wall_sign * double(cs.y()) : 0.0;
            const Point  sp    = Point::new_scale(x, y);
            np.append(Point3(sp.x(), sp.y(), coord_t(scale_(taper * (woven + brick_off)))));
            if (nl != nullptr)
                record(sp, cs);
        };

        const Vec2d  c0(unscale_(pts[0].x()), unscale_(pts[0].y()));
        const double taper0 = edge_taper(wp.layer, Point(pts[0].x(), pts[0].y()), wp.taper_k);
        if (have2)
            s_run += (c0 - prev2).norm(); // shared joint between paths of the same loop
        prev2 = c0; have2 = true; taper_prev = taper0;
        emit(c0.x(), c0.y(), taper0, s_run);

        for (size_t j = 1; j < pts.size(); ++j) {
            const Vec2d  cur(unscale_(pts[j].x()), unscale_(pts[j].y()));
            const double seglen = (cur - prev2).norm();
            if (seglen < 1e-9) {
                prev2 = cur;
                continue;
            }
            const double taper_cur = edge_taper(wp.layer, Point(pts[j].x(), pts[j].y()), wp.taper_k);
            // Only the woven sinusoid needs fine subdivision. A brick (constant Z + linear
            // taper) is interpolated fine by a plain G1, so keep the original wall points —
            // otherwise the gcode explodes (~5x) and the preview runs out of memory.
            const int    nseg      = wp.woven ? std::max(1, int(std::ceil(seglen / wp.res))) : 1;
            const double s_base    = s_run;
            for (int kk = 1; kk <= nseg; ++kk) {
                const double t  = double(kk) / nseg;
                const Vec2d  pt = prev2 + (cur - prev2) * t;
                const double tp = taper_prev + (taper_cur - taper_prev) * t;
                emit(pt.x(), pt.y(), tp, s_base + seglen * t);
            }
            s_run      = s_base + seglen;
            prev2      = cur;
            taper_prev = taper_cur;
        }
        p.polyline       = std::move(np);
        p.z_contoured    = true;
        p.z_no_flow_comp = wp.brick; // brick = rigid shift, keep nominal flow
        if (wp.flow_ratio != 1.0)
            p.mm3_per_mm *= wp.flow_ratio; // extra flow on the reshaped wall only
        if (wp.brick && brick_off != 0.0 && wp.brick_flow != 1.0)
            p.mm3_per_mm *= wp.brick_flow; // pack the shifted brick interface (TengerTechnologies-style)
    }
    return true;
}

// inset_idx is the wall index from the OUTSIDE (0 = outermost external wall).
bool weaveable(const ExtrusionEntity *e, const WeaveParams &wp)
{
    if (!is_perimeter(e->role()) || is_bridge(e->role()))
        return false;
    const int d = e->inset_idx;
    if (d >= 0) {
        if (d < wp.skip_outer)
            return false; // keep the N outermost walls planar
        if (wp.wall_count > 0 && d >= wp.wall_count - wp.skip_inner)
            return false; // keep the M innermost walls planar
    }
    return true;
}

void weave_entity(ExtrusionEntity *e, const WeaveParams &wp);

void weave_collection(ExtrusionEntityCollection &coll, const WeaveParams &wp)
{
    for (ExtrusionEntity *e : coll.entities)
        weave_entity(e, wp);
}

void weave_entity(ExtrusionEntity *e, const WeaveParams &wp)
{
    // Leave scarf-seam (sloped) entities planar — they own the Z near the seam.
    if (dynamic_cast<ExtrusionLoopSloped *>(e) || dynamic_cast<ExtrusionPathSloped *>(e))
        return;
    if (auto *loop = dynamic_cast<ExtrusionLoop *>(e)) {
        if (weaveable(loop, wp))
            weave_path_sequence(loop->paths, wp, loop->inset_idx);
        return;
    }
    if (auto *mp = dynamic_cast<ExtrusionMultiPath *>(e)) {
        if (weaveable(mp, wp))
            weave_path_sequence(mp->paths, wp, mp->inset_idx);
        return;
    }
    if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(e)) {
        weave_collection(*coll, wp);
        return;
    }
    if (auto *path = dynamic_cast<ExtrusionPath *>(e)) {
        if (weaveable(path, wp)) {
            std::vector<ExtrusionPath> one{*path};
            if (weave_path_sequence(one, wp, path->inset_idx))
                *path = one.front();
        }
        return;
    }
}

} // namespace

void Layer::make_woven_walls(WovenPhaseField &prev)
{
    // One distancer per loop on the layer below, so each loop matches its own counterpart
    // (no cross-feature contamination). Empty on the first woven layer -> seed.
    std::vector<AABBTreeLines::LinesDistancer<Line>> prev_dists;
    prev_dists.reserve(prev.loops.size());
    for (const WovenLoopField &lp : prev.loops)
        prev_dists.emplace_back(lp.lines);

    WovenPhaseField next; // accumulate THIS layer's phase field for the layer above

    // One claim flag per below-loop, shared across all regions of this layer so the greedy
    // area-aware feature match (Step A) can't bind two of this layer's loops to the same one.
    std::vector<char> claimed(prev.loops.size(), 0);

    for (LayerRegion *region : this->regions()) {
        const PrintRegionConfig &cfg = region->region().config();
        // Woven and brick can run together: woven gives the sinusoid, brick adds a constant
        // Z stagger between adjacent shells.
        const bool brick = cfg.brick_layers_enabled;
        if (!cfg.woven_walls_enabled && !brick)
            continue;
        const int wall_count = cfg.wall_loops.value;
        const int skip_outer = std::max(0, cfg.woven_wall_skip_outer.value);
        const int skip_inner = std::max(0, cfg.woven_wall_skip_inner.value);
        if (wall_count > 0 && skip_outer + skip_inner >= wall_count)
            continue;

        const bool nested    = cfg.woven_walls_nested;
        const bool interwall = cfg.woven_walls_interwall;

        double amp_frac = std::max(0.0, std::min(0.45, cfg.woven_wall_amplitude.value));
        // Antiphase (layer-to-layer or wall-to-wall) swings the inter-bead gap by 2*amp;
        // clamp to keep within max_sep and avoid lens-shaped voids.
        if (nested || interwall) {
            const double max_amp = cfg.woven_wall_max_sep.value / 2.0;
            if (amp_frac > max_amp)
                amp_frac = max_amp;
        }

        WeaveParams wp;
        wp.amp_mm       = amp_frac * this->height;
        wp.wavelength   = std::max(0.5, cfg.woven_wall_wavelength.value);
        wp.sign         = nested ? ((this->id() % 2 == 0) ? 1.0 : -1.0) : 1.0;
        // 8 points per wavelength keeps the sine's chord error under ~10% of amplitude
        // (invisible at a sub-100um weave) while halving the gcode size + print time vs the
        // old /16. Coarser still would face the wave; finer just bloats a huge part's gcode.
        wp.res          = std::max(0.05, std::min(wp.wavelength / 8.0, 0.6));
        wp.layer        = this;
        wp.taper_k      = cfg.woven_wall_edge_taper.value;
        // Interior-layer fast path: skip the per-point taper test (expensive point-in-
        // polygon) when the cross-section is unchanged K layers above AND below.
        if (wp.taper_k > 0) {
            const double a0 = layer_lslices_area(this);
            const Layer *up = this, *dn = this;
            for (int i = 0; i < wp.taper_k && up; ++i) up = up->upper_layer;
            for (int i = 0; i < wp.taper_k && dn; ++i) dn = dn->lower_layer;
            if (up && dn && a0 > 0.0 &&
                std::abs(layer_lslices_area(up) - a0) < 0.01 * a0 &&
                std::abs(layer_lslices_area(dn) - a0) < 0.01 * a0)
                wp.taper_k = 0;
        }
        wp.interwall    = interwall;
        wp.brick_off_mm = std::max(0.0, std::min(0.5, cfg.brick_layer_offset.value)) * this->height;
        wp.brick_flow   = cfg.brick_layer_flow.value;
        wp.skip_outer   = skip_outer;
        wp.skip_inner   = skip_inner;
        wp.wall_count   = wall_count;
        wp.flow_ratio   = cfg.woven_wall_flow_ratio.value;
        wp.woven        = cfg.woven_walls_enabled && wp.amp_mm >= 1e-4;
        wp.brick        = brick && wp.brick_off_mm >= 1e-4;
        wp.prev         = prev.empty() ? nullptr : &prev;
        wp.prev_dists   = prev.empty() ? nullptr : &prev_dists;
        wp.next         = &next;
        wp.claimed      = prev.empty() ? nullptr : &claimed;
        if (!wp.woven && !wp.brick)
            continue;

        weave_collection(region->perimeters, wp);
    }
    prev = std::move(next); // hand this layer's field up to the next layer
}

} // namespace Slic3r
