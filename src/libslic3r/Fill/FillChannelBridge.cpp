#include "FillChannelBridge.hpp"

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Surface.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

// One centreline piece: points with a channel width per point.
struct Centreline
{
    std::vector<Vec2d>  pts;
    std::vector<double> w;
    bool                closed = false;

    double length() const
    {
        double l = 0;
        for (size_t i = 1; i < pts.size(); ++i)
            l += (pts[i] - pts[i - 1]).norm();
        return l;
    }
    void reverse()
    {
        std::reverse(pts.begin(), pts.end());
        std::reverse(w.begin(), w.end());
    }
};

// MedialAxis stores the width at the START and END of every segment (2 * (n - 1) entries for n
// points); average the two that meet at an interior point. Tolerate a per-point vector too.
double width_at(const ThickPolyline &tp, size_t i)
{
    const size_t n = tp.points.size();
    if (n > 1 && tp.width.size() == 2 * n - 2) {
        if (i == 0)     return tp.width.front();
        if (i >= n - 1) return tp.width.back();
        return 0.5 * (tp.width[2 * i - 1] + tp.width[2 * i]);
    }
    if (i < tp.width.size()) return tp.width[i];
    return tp.width.empty() ? 0.0 : tp.width.back();
}

Centreline to_axis(const ThickPolyline &tp)
{
    Centreline a;
    a.pts.reserve(tp.points.size());
    a.w.reserve(tp.points.size());
    for (size_t i = 0; i < tp.points.size(); ++i) {
        a.pts.emplace_back(tp.points[i].cast<double>());
        a.w.emplace_back(width_at(tp, i));
    }
    return a;
}

// The Voronoi walk hands back a curved channel's centreline in pieces: it stops wherever an edge
// was pruned or a corner spur branched off. Join the nearest free ends back together (a straight
// hop across the break; spokes are clipped to the surface, so a hop over nothing costs nothing)
// and recognise a loop when a piece meets itself.
void stitch(std::vector<Centreline> &axes, double tol)
{
    const double tol2 = tol * tol;
    for (;;) {
        double best = tol2;
        size_t bi = 0, bj = 0; int mode = -1;
        for (size_t i = 0; i < axes.size(); ++i) {
            if (axes[i].closed) continue;
            for (size_t j = i + 1; j < axes.size(); ++j) {
                if (axes[j].closed) continue;
                const Centreline &a = axes[i], &b = axes[j];
                const double d[4] = { (a.pts.back()  - b.pts.front()).squaredNorm(),    // a -> b
                                      (a.pts.back()  - b.pts.back()).squaredNorm(),     // a -> rev b
                                      (a.pts.front() - b.pts.front()).squaredNorm(),    // rev a -> b
                                      (a.pts.front() - b.pts.back()).squaredNorm() };   // rev a -> rev b
                for (int m = 0; m < 4; ++m)
                    if (d[m] <= best) { best = d[m]; bi = i; bj = j; mode = m; }
            }
        }
        if (mode < 0)
            break;
        Centreline &a = axes[bi];
        Centreline  b = std::move(axes[bj]);
        if (mode == 2 || mode == 3) a.reverse();
        if (mode == 1 || mode == 3) b.reverse();
        const size_t skip = (a.pts.back() - b.pts.front()).squaredNorm() == 0 ? 1 : 0;
        a.pts.insert(a.pts.end(), b.pts.begin() + skip, b.pts.end());
        a.w.insert(a.w.end(), b.w.begin() + skip, b.w.end());
        axes.erase(axes.begin() + bj);
    }
    for (Centreline &a : axes)
        if (a.pts.size() > 2 && (a.pts.front() - a.pts.back()).squaredNorm() <= tol2) {
            if ((a.pts.front() - a.pts.back()).squaredNorm() > 0) {
                a.pts.push_back(a.pts.front());
                a.w.push_back(a.w.front());
            }
            a.closed = true;
        }
}

// One spoke through p, normal to t, half-length `half` each side, clipped to the surface. The
// clipped piece nearest p is the crossing. Oriented from the p - n side to the p + n side so
// consecutive spokes can be zigzagged.
bool cast_spoke(const ExPolygon &expoly, const Vec2d &p, const Vec2d &t, double half, Polyline &out)
{
    const Vec2d n(-t.y(), t.x());
    const Vec2d a = p - n * half, b = p + n * half;
    Polyline spoke;
    spoke.points.emplace_back(Point(a.cast<coord_t>()));
    spoke.points.emplace_back(Point(b.cast<coord_t>()));
    Polylines clipped = intersection_pl(spoke, expoly);
    if (clipped.empty())
        return false;
    Polyline *best = &clipped.front();
    double    bestd = std::numeric_limits<double>::max();
    for (Polyline &c : clipped) {
        double d = std::numeric_limits<double>::max();
        for (size_t i = 1; i < c.points.size(); ++i)
            d = std::min(d, Line(c.points[i - 1], c.points[i]).distance_to(Point(p.cast<coord_t>())));
        if (d < bestd) { bestd = d; best = &c; }
    }
    out = std::move(*best);
    if ((out.first_point().cast<double>() - a).squaredNorm() > (out.last_point().cast<double>() - a).squaredNorm())
        out.reverse();
    return out.points.size() >= 2;
}

} // namespace

Polylines FillChannelBridge::fill_surface(const Surface *surface, const FillParams &params)
{
    // The same inset FillBase applies before any fill, so spokes end where rectilinear lines would:
    // inside the anchor-expanded bridge surface, on solid material.
    ExPolygons expp = offset_ex(surface->expolygon, float(scale_(this->overlap - 0.5 * this->spacing)));
    Polylines out;
    bool all_channels = !expp.empty();
    for (const ExPolygon &expoly : expp)
        if (!this->fill_channel(expoly, params, out)) {
            all_channels = false;
            break;
        }
    double a = 0;
    for (const ExPolygon &e : expp) a += e.area();
    BOOST_LOG_TRIVIAL(info) << "ATN channel-bridge: z " << this->z << " expolys " << expp.size()
                            << " area_mm2 " << a * SCALING_FACTOR * SCALING_FACTOR
                            << " -> " << (all_channels ? "SPOKES, chains " + std::to_string(out.size()) : "fallback rectilinear");
    if (all_channels)
        return out;
    // Not a channel: the ordinary single-angle bridge, untouched.
    return FillRectilinear::fill_surface(surface, params);
}

bool FillChannelBridge::fill_channel(const ExPolygon &expoly, const FillParams &params, Polylines &out) const
{
    // Spoke spacing along the centreline: the bridge line pitch (density is 0..1 here).
    const double pitch = scale_(this->spacing / std::max(0.05f, params.density));
    const BoundingBox bb = get_extents(expoly);
    const double diag = std::hypot(double(bb.size().x()), double(bb.size().y()));

    // The raw skeleton. Not ExPolygon::medial_axis(): that extends every piece end along its
    // tangent until it hits the contour, which on a curved channel drags piece ends off the
    // centreline and into the wall.
    ThickPolylines raw;
    Geometry::MedialAxis(scale_(this->spacing), diag, expoly).build(&raw);
    std::vector<Centreline> axes;
    for (const ThickPolyline &tp : raw)
        if (tp.points.size() >= 2)
            axes.push_back(to_axis(tp));
    const size_t n_raw = axes.size();
    if (axes.empty()) {
        BOOST_LOG_TRIVIAL(info) << "ATN channel-bridge:   bail: empty medial axis (bbox "
                                << unscaled(bb.size().x()) << "x" << unscaled(bb.size().y()) << " mm)";
        return false;
    }

    // Spurs: the skeleton grows a short branch into every corner. Cut anything shorter than twice
    // the typical width, then stitch what is left back into continuous centrelines.
    double wsum = 0; size_t wn = 0;
    for (const Centreline &a : axes)
        for (double w : a.w) { wsum += w; ++wn; }
    const double wmean0 = wn ? wsum / double(wn) : 0.0;
    if (wmean0 <= 0)
        return false;
    axes.erase(std::remove_if(axes.begin(), axes.end(), [wmean0](const Centreline &a) { return a.length() < 2.0 * wmean0; }), axes.end());
    const size_t n_cut = axes.size();
    stitch(axes, 2.0 * wmean0);
    if (axes.empty()) {
        BOOST_LOG_TRIVIAL(info) << "ATN channel-bridge:   bail: nothing left after spur cut (raw " << n_raw << ", wmean " << unscaled(wmean0) << " mm)";
        return false;
    }

    // Channel test: long relative to its width, and the width barely varies along it.
    wsum = 0; wn = 0; double wsq = 0, len = 0; size_t n_closed = 0;
    for (const Centreline &a : axes) {
        len += a.length();
        n_closed += a.closed;
        for (double w : a.w) { wsum += w; wsq += w * w; ++wn; }
    }
    const double wmean = wsum / double(wn);
    const double wcv   = std::sqrt(std::max(0.0, wsq / double(wn) - wmean * wmean)) / wmean;
    if (len < 3.0 * wmean || wcv > 0.35) {
        BOOST_LOG_TRIVIAL(info) << "ATN channel-bridge:   bail: not channel-shaped: len " << unscaled(len) << " mm, wmean "
                                << unscaled(wmean) << " mm, width CV " << wcv << " (pieces raw " << n_raw << " cut " << n_cut
                                << " stitched " << axes.size() << ")";
        return false;
    }

    const size_t out_before = out.size();
    size_t n_spokes = 0, n_ext = 0;
    for (const Centreline &ax : axes) {
        std::vector<Polyline> spokes;                                   // in order along the centreline
        double carry = 0.5 * pitch;                                     // distance along the axis to the next spoke
        for (size_t i = 0; i + 1 < ax.pts.size(); ++i) {
            const Vec2d  d   = ax.pts[i + 1] - ax.pts[i];
            const double seg = d.norm();
            if (seg <= 0.0)
                continue;
            const Vec2d t = d / seg;
            while (carry <= seg) {
                const double u = carry / seg;
                const double w = ax.w[i] + (ax.w[i + 1] - ax.w[i]) * u;
                Polyline s;
                if (cast_spoke(expoly, ax.pts[i] + d * u, t, w + pitch, s))   // overshoot the walls, then clip
                    spokes.emplace_back(std::move(s));
                carry += pitch;
            }
            carry -= seg;
        }
        // Open ends: the skeleton stops about half a width short of an end wall. Keep casting along
        // the end tangent while the spoke still spans the channel and its centre is still inside.
        if (!ax.closed && ax.pts.size() >= 2) {
            const size_t n     = ax.pts.size();
            const size_t max_k = size_t(std::ceil(2.0 * wmean / pitch));
            auto extend = [&](const Vec2d &end, const Vec2d &step, const Vec2d &basis, double w, std::vector<Polyline> &dst) {
                for (size_t k = 1; k <= max_k; ++k) {
                    const Vec2d q = end + step * (double(k) * pitch);
                    if (!expoly.contains(Point(q.cast<coord_t>())))
                        break;
                    Polyline s;
                    if (!cast_spoke(expoly, q, basis, w + pitch, s) || s.length() < 0.6 * w)
                        break;
                    dst.emplace_back(std::move(s));
                }
            };
            std::vector<Polyline> front, back;
            Vec2d t0 = ax.pts[1] - ax.pts[0];
            if (t0.norm() > 0) { t0.normalize(); extend(ax.pts[0], -t0, t0, ax.w[0], front); }
            Vec2d t1 = ax.pts[n - 1] - ax.pts[n - 2];
            if (t1.norm() > 0) { t1.normalize(); extend(ax.pts[n - 1], t1, t1, ax.w[n - 1], back); }
            n_ext += front.size() + back.size();
            std::vector<Polyline> ordered;
            ordered.reserve(front.size() + spokes.size() + back.size());
            for (auto it = front.rbegin(); it != front.rend(); ++it) ordered.emplace_back(std::move(*it));
            for (Polyline &s : spokes) ordered.emplace_back(std::move(s));
            for (Polyline &s : back)   ordered.emplace_back(std::move(s));
            spokes = std::move(ordered);
        }
        // Zigzag: alternate direction and join consecutive spokes along the wall; start a new chain
        // where the hop is not a wall-hugging step (a gap in the channel).
        Polyline chain;
        Point    prev_end;
        bool     have_prev = false, flip = false;
        for (Polyline &s : spokes) {
            if (flip)
                s.reverse();
            flip = !flip;
            if (have_prev && (s.first_point() - prev_end).cast<double>().norm() > 2.5 * pitch) {
                if (chain.points.size() > 1)
                    out.emplace_back(std::move(chain));
                chain = Polyline();
            }
            chain.points.insert(chain.points.end(), s.points.begin(), s.points.end());
            prev_end  = s.last_point();
            have_prev = true;
            ++n_spokes;
        }
        if (chain.points.size() > 1)
            out.emplace_back(std::move(chain));
    }
    BOOST_LOG_TRIVIAL(info) << "ATN channel-bridge:   channel: pieces raw " << n_raw << " cut " << n_cut << " stitched " << axes.size()
                            << " (closed " << n_closed << "), len " << unscaled(len) << " mm, wmean " << unscaled(wmean)
                            << " mm, CV " << wcv << ", spokes " << n_spokes << " (end extensions " << n_ext << "), chains "
                            << (out.size() - out_before);
    return out.size() > out_before;
}

} // namespace Slic3r
