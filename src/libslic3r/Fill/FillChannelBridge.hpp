#ifndef slic3r_FillChannelBridge_hpp_
#define slic3r_FillChannelBridge_hpp_

#include "FillRectilinear.hpp"

namespace Slic3r {

// ATN: bridge fill that follows the LOCAL shortest span.
//
// Orca picks ONE bridge angle per surface (BridgeDetector scores a few candidate directions by
// anchored length). On a bridge over a CHANNEL -- an O-ring groove printed groove-down, or any
// curved gap of near-constant width -- that single angle is radial at two points of the ring and
// tangential at the two points 90 degrees round, where the lines run ALONG the gap, unsupported,
// over the longest possible chord. No global angle is right everywhere on a curve.
//
// This fill builds the surface's medial axis (its centreline, with the local width at every
// point), walks it at the bridge line pitch, and lays one spoke across the channel at every
// step, perpendicular to the centreline, clipped to the anchor-expanded surface so each spoke
// lands on solid material at BOTH ends. Consecutive spokes alternate direction and are joined
// along the wall -- a zigzag that marches round the arc with the nozzle never travelling over
// air. Every spoke spans exactly the local gap: the best case bridging ever gets, everywhere on
// the ring, for the same extruded length as the single-angle fill.
//
// It engages only when the surface IS a channel (a single centreline, near-constant width, long
// relative to that width). Anything else -- and any piece it cannot classify -- falls through to
// FillRectilinear unchanged, so a bridge that is not channel-shaped prints exactly as before.
class FillChannelBridge : public FillRectilinear
{
public:
    Fill* clone() const override { return new FillChannelBridge(*this); }
    ~FillChannelBridge() override = default;
    Polylines fill_surface(const Surface *surface, const FillParams &params) override;

private:
    // Spokes for one expolygon; false when it is not a channel (nothing is appended then).
    bool fill_channel(const ExPolygon &expoly, const FillParams &params, Polylines &out) const;
};

} // namespace Slic3r

#endif // slic3r_FillChannelBridge_hpp_
