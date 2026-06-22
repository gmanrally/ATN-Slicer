#ifndef slic3r_WovenWalls_hpp_
#define slic3r_WovenWalls_hpp_

#include "Point.hpp"
#include "Line.hpp"
#include <vector>

namespace Slic3r {

// One perimeter loop's woven-wall phase samples, handed UP to the next layer. The phase is
// a (cos,sin) unit vector per segment endpoint (seam-wrap-safe, interpolable). `centroid`+
// `area` identify the feature across layers (gated, area-aware, claimed match) so each loop
// propagates from ITS OWN counterpart below (no cross-feature contamination / moire).
struct WovenLoopField {
    Vec2d              centroid {Vec2d::Zero()};
    double             area     {0.0};          // for area-aware feature tracking (+ birth radius)
    bool               round    {false};        // surface of revolution -> fixed-axis radial
    Vec2d              axis     {Vec2d::Zero()}; // fixed angle centre (round loops)
    int                ncyc     {1};            // fixed cycle count (round loops)
    double             L        {0.0};          // perimeter (hysteresis / re-anchor wave count)
    int                n_wall   {1};            // developable seed wave count, inherited up a feature
    std::vector<Line>  lines;     // this loop's woven wall segments (scaled XY)
    std::vector<Vec2f> cs0, cs1;  // base phase (cos, sin) at each segment's start / end
};

// All loops of one layer, for the layer above to register against (phase propagation).
struct WovenPhaseField {
    std::vector<WovenLoopField> loops;

    bool empty() const { return loops.empty(); }
    void clear() { loops.clear(); }
};

} // namespace Slic3r

#endif
