// Adapted from BambuStudio, AGPL-3.0, commit 56a57d00a591360cf41bf21383915ee5bf357be5.
#pragma once

#include "ExPolygon.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

class Layer;
class PrintConfig;
class PrintObject;
struct SlicingParameters;

// Filament ids are one-based. Zero entries inherit the scalar process height. Short vectors broadcast their first entry.
bool has_extruder_layer_heights(const PrintConfig &config);
bool has_extruder_layer_heights(const PrintConfig &config, const std::vector<unsigned int> &used_filaments);
bool has_extruder_layer_heights(const PrintObject &object);
bool independent_support_layers(const PrintConfig &config, const SlicingParameters &params);
double extruder_layer_height_for_filament(const PrintConfig &config, unsigned int filament, double fallback);
double extruder_layer_height_grid(const PrintConfig &config, double fallback, const std::vector<unsigned int> &used_filaments);
std::vector<double> extruder_support_layer_steps(double span, double height, double minimum);


// Retain the original layout across individual manual edits until they form a permutation.
// Diameter telemetry cannot distinguish equal-size nozzles: unchanged slots take priority.
class ExtruderLayerHeightRemapper {
public:
    void reset(const std::vector<double> &diameters, const std::vector<double> &heights);
    std::vector<double> remap(const std::vector<double> &diameters, const std::vector<double> &heights);

private:
    std::vector<double> m_diameters;
    std::vector<double> m_last_diameters;
    std::vector<double> m_heights;
};

struct AdaptiveLayerHeightRange
{
    double min_height { 0. };
    double max_height { 0. };

    bool contains(double height) const;
};

struct AdaptiveMergePatch
{
    // A region may carry multiple disjoint patches on one group-top layer. They keep the nozzle's common
    // top_idx but may start at different bottom_idx values after local Z-conflict clipping.
    size_t     bottom_idx { 0 };
    size_t     top_idx { 0 };
    // Only set on the copy owned by the group-top LayerRegion. Layer::id() may be offset against the
    // PrintObject layer index (raft layers, bottom layers dropped by fix_slicing_errors), so the role
    // has to be stored explicitly instead of being derived from top_idx.
    bool       group_top { false };
    double     merged_height { 0. };

    // The retained contour is extruded on the group top and marks the swallowed footprint on lower layers.
    const ExPolygons &area() const
    {
        static const ExPolygons empty;
        return m_area ? *m_area : empty;
    }

    void set_area(ExPolygons area) { m_area = std::make_shared<const ExPolygons>(std::move(area)); }

    // Geometry that could not be assigned to a printable slab. It is intentionally separate from area():
    // the patch must suppress fill in this footprint without generating walls or claiming Z occupancy.
    const ExPolygons &suppressed_area() const
    {
        static const ExPolygons empty;
        return m_suppressed_area ? *m_suppressed_area : empty;
    }

    void set_suppressed_area(ExPolygons area)
    {
        m_suppressed_area = std::make_shared<const ExPolygons>(std::move(area));
    }

    bool has_suppressed_area() const { return bool(m_suppressed_area); }

private:
    // All copies stored across a merge span share the same immutable geometry.
    std::shared_ptr<const ExPolygons> m_area;
    std::shared_ptr<const ExPolygons> m_suppressed_area;
};

// One band of the layer stack, [bottom_idx, top_idx] inclusive. A nozzle prints the band as a single
// extrusion of merged_height, placed on the top layer; the layers below it are dropped.
struct AdaptiveLayerBand
{
    // Ordinary bands are shaped by top_idx. The first band is special: callers use the true first-layer
    // contour and color, and top_idx only selects the physical Z where that snapshot is extruded.
    size_t bottom_idx { 0 };
    size_t top_idx { 0 };
    double merged_height { 0. };
};

AdaptiveLayerHeightRange extruder_layer_height_range(const PrintConfig &config, unsigned int filament);

// ---- Bambu Studio 2.9 mixed nozzle diameters ---------------------------------------------------------------------
// The printable layer height window of a nozzle diameter, derived from the diameter itself:
// 0.2 -> 0.04..0.14, 0.4 -> 0.08..0.28, 0.6 -> 0.12..0.42, 0.8 -> 0.16..0.56.
AdaptiveLayerHeightRange adaptive_layer_height_range(double nozzle_diameter);
// Top gap between support and the object above it for a nozzle diameter. Only 0.2 / 0.4 / 0.6 / 0.8 ship; anything
// else takes the nearest one.
float support_top_z_for_nozzle(double nozzle_diameter);
// True when the filaments handed in (0-based) do not all share one nozzle diameter.
bool has_multiple_used_nozzle_diameters(const PrintConfig &print_config, const std::vector<unsigned int> &filament_ids);
// How many base layers one nozzle merges into a single extrusion: the diameter ratio against the grid nozzle, corrected
// when the resulting thickness falls outside what the nozzle can extrude. 1 when it has to print every layer.
size_t adaptive_layer_band_size(double nozzle_diameter, double min_nozzle_diameter, double base_layer_height);
// Layer height a 1-based filament prints at under multi_nozzle_diameter_mode when no explicit extruder_layer_height is
// set: its nozzle's band on the 0.2 mm-anchored grid of the authored layer height. 0 while the mode is off.
double derived_nozzle_layer_height(const PrintConfig &config, unsigned int filament, double authored_layer_height);

// Keep only the pieces a nozzle of this diameter could really lay a bead into. Two contours polygonised
// apart - consecutive slices of a shrinking shape, a slice and the paint covering it - never coincide point
// for point, and what one has over the other is a fringe of degenerate, near zero area slivers. Offsetting a
// degenerate contour makes Clipper hand back islands orders of magnitude larger than what it was given, so
// the area test comes first and the erosion is read as a yes/no answer only: the returned pieces are always
// the originals, never the output of the offset.
ExPolygons adaptive_printable_pieces(ExPolygons pieces, double nozzle_diameter);

// Like adaptive_printable_pieces(), but also trims every kept piece down to the part a bead of this nozzle
// fits into. A sloped wall's fringe that widens at one corner passes the yes/no test as a whole, and its
// hairline tails would otherwise become a patch of their own and be walled as fragmented thin lines.
ExPolygons adaptive_printable_core(ExPolygons pieces, double nozzle_diameter);

// The footprint of a layer whose orphans were suppressed: sliced, but extruded by no region. What rests on it
// is not supported there.
ExPolygons adaptive_suppressed_orphan_area(const Layer &layer);

// Keep the common first layer, then split the remaining stack into bands of band_size layers. The first layer starts at the object base
// and is extruded on its top layer with the complete merged height, so it rests on the bed or on the
// object's raft/support substrate. Every nozzle shares that anchor, so bands of commensurate sizes nest
// into each other and their boundaries never straddle.
//
// band_tops optionally marks hard structural boundaries that a band must not straddle. Colour ownership
// changes are deliberately not such boundaries: every nozzle keeps its fixed ladder and the caller aligns
// ownership to the band's anchor contour. The marks are honored unless doing so would leave a segment the
// nozzle cannot extrude, in which case printability wins. Pass an empty vector for the normal fixed grid.
//
// Unlike a fixed stride, no layer is left out: a tail too short for a full band is folded back into the
// segment before it and the segment is re-cut, so eight layers with band_size five come out as 4 + 4 rather
// than 5 + an orphaned 3. Returns an empty plan when band_size is 1, i.e. when this nozzle merges nothing.
std::vector<AdaptiveLayerBand> build_adaptive_layer_bands(
    const std::vector<double>      &layer_heights,
    size_t                          band_size,
    const AdaptiveLayerHeightRange &height_range,
    const std::vector<uint8_t>     &band_tops = {});

// Tail recovery may pin a band top on a layer this nozzle never owns (the geometric stack top after
// a colour disappears). Pull top_idx down to the last layer in the band that still has material,
// and recompute merged_height. Returns false when no such layer exists, the leftover is a single
// step, or the shortened height is outside the printable window: the caller then abandons the band
// instead of falling back to the base layer height on every step.
//
// has_material is indexed by layer; it must cover [bottom_idx, top_idx].
bool pull_adaptive_band_top_to_material(
    AdaptiveLayerBand                &band,
    const std::vector<double>        &layer_heights,
    const AdaptiveLayerHeightRange   &height_range,
    const std::vector<unsigned char> &has_material);

bool split_adaptive_layer_span(
    const std::vector<double>              &layer_heights,
    size_t                                  bottom_idx,
    size_t                                  top_idx,
    size_t                                  preferred_band_size,
    const AdaptiveLayerHeightRange         &height_range,
    std::vector<std::pair<size_t, size_t>> &groups);

} // namespace Slic3r
