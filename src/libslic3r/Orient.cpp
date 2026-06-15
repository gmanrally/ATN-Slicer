#include "Orient.hpp"
#include "Geometry.hpp"
#include <cfloat>
#include <cmath>
#include <numeric>
#include <ClipperUtils.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/multiprecision/integer.hpp>
#include <boost/rational.hpp>

#undef MAX3
#define MAX3(a,b,c) std::max(std::max(a,b),c)

#undef MEDIAN
#define MEDIAN3(a,b,c) std::max(std::min(a,b), std::min(std::max(a,b),c))
#ifndef SQ
#define SQ(x) ((x)*(x))
#endif

namespace Slic3r {

namespace orientation {

    struct CostItems {
        float overhang;
        float bottom;
        float bottom_hull;
        float contour;
        float area_laf;  // area_of_low_angle_faces
        float area_projected; // area of projected 2D profile
        float area_support; // ATN: true surface area (mm^2) needing support at this orientation
        float support_volume; // ATN: swept support volume (mm^3) under overhangs to the bed
        float height;       // ATN: build height (mm) along the orientation axis
        float footprint_excess; // ATN: mm by which the XY footprint overflows the bed (0 = fits)
        float volume;
        float area_total;  // total area of all faces
        float radius;    // radius of bounding box
        float height_to_bottom_hull_ratio;  // affects stability, the lower the better
        float unprintability;
        CostItems(CostItems const & other) = default;
        CostItems() { memset(this, 0, sizeof(*this)); }
        static std::string field_names() {
            return "                                      A_support, bottom, height, fit_excess, unprintability";
        }
        std::string field_values() {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1);
            ss << area_support << ",\t" << bottom << ",\t" << height << ",\t" << footprint_excess << ",\t" << unprintability;
            return ss.str();
        }
    };



// A class encapsulating the libnest2d Nester class and extending it with other
// management and spatial index structures for acceleration.
class AutoOrienter {
public:
    int face_count_hull;
    OrientMesh *orient_mesh = NULL;
    TriangleMesh* mesh;
    TriangleMesh mesh_convex_hull;
    Eigen::MatrixXf normals, normals_quantize, normals_hull, normals_hull_quantize;
    Eigen::VectorXf areas, areas_hull;
    Eigen::VectorXf is_apperance; // whether a facet is outer apperance
    Eigen::MatrixXf z_projected;
    Eigen::VectorXf z_max, z_max_hull;  // max of projected z
    Eigen::VectorXf z_median;  // median of projected z
    Eigen::VectorXf z_mean;  // mean of projected z
    std::vector<Vec3f> face_normals;
    std::vector<Vec3f> face_normals_hull;
    OrientParams params;


    std::vector< Vec3f> orientations;  // Vec3f == stl_normal
    std::function<void(unsigned)> progressind = { };  // default empty indicator function

public:
    AutoOrienter(OrientMesh* orient_mesh_,
                 const OrientParams           &params_,
                 std::function<void(unsigned)> progressind_,
                 std::function<bool(void)>     stopcond_)
    {
        orient_mesh = orient_mesh_;
        mesh = &orient_mesh->mesh;
        params = params_;
        progressind = progressind_;
        params.ASCENT = cos(PI - orient_mesh->overhang_angle * PI / 180); // use per-object overhang angle
        
        // BOOST_LOG_TRIVIAL(info) << orient_mesh->name << ", angle=" << orient_mesh->overhang_angle << ", params.ASCENT=" << params.ASCENT;
        // std::cout << orient_mesh->name << ", angle=" << orient_mesh->overhang_angle << ", params.ASCENT=" << params.ASCENT;

        preprocess();
    }

    AutoOrienter(TriangleMesh* mesh_)
    {
        mesh = mesh_;
        preprocess();
    }

    struct VecHash {
        size_t operator()(const Vec3f& n1) const {
            return std::hash<coord_t>()(int(n1(0)*100+100)) + std::hash<coord_t>()(int(n1(1)*100+100)) * 101 + std::hash<coord_t>()(int(n1(2)*100+100)) * 10221;
        }
    };

    Vec3f quantize_vec3f(const Vec3f n1) {
        return Vec3f(floor(n1(0) * 1000) / 1000, floor(n1(1) * 1000) / 1000, floor(n1(2) * 1000) / 1000);
    }

    Vec3d process()
    {
        orientations = { { 0,0,-1 } }; // original orientation

        area_cumulation_accurate(face_normals, normals_quantize, areas, 10);

        area_cumulation_accurate(face_normals_hull, normals_hull_quantize, areas_hull, 14);

        add_supplements();

        if(progressind)
            progressind(20);

        remove_duplicates();

        if (progressind)
            progressind(30);

        std::unordered_map<Vec3f, CostItems, VecHash> results;
        BOOST_LOG_TRIVIAL(info) << CostItems::field_names();
        std::cout << CostItems::field_names() << std::endl;
        for (int i = 0; i < orientations.size();i++) {
            Vec3f orientation = -orientations[i];

            project_vertices(orientation);

            auto cost_items = get_features(orientation, params.min_volume);

            float unprintability = target_function(cost_items, params.min_volume);

            results[orientation] = cost_items;

            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(4) << "orientation:" << orientation.transpose() << ", cost:" << std::fixed << std::setprecision(4) << cost_items.field_values();
            std::cout << std::fixed << std::setprecision(4) << "orientation:" << orientation.transpose() << ", cost:" << std::fixed << std::setprecision(4) << cost_items.field_values() << std::endl;
        }
        if (progressind)
            progressind(60);

        typedef std::pair<Vec3f, CostItems> PAIR;
        std::vector<PAIR> results_vector(results.begin(), results.end());
        sort(results_vector.begin(), results_vector.end(), [](const PAIR& p1, const PAIR& p2) {return p1.second.unprintability < p2.second.unprintability; });

        if (progressind)
            progressind(80);

        //To avoid flipping, we need to verify if there are orientations with same unprintability.
        Vec3f n1 = {0, 0, 1};
        auto best_orientation = results_vector[0].first;

        for (int i = 1; i< results_vector.size()-1; i++) {
            if (abs(results_vector[i].second.unprintability - results_vector[0].second.unprintability) < EPSILON && abs(results_vector[0].first.dot(n1)-1) > EPSILON) {
                if (abs(results_vector[i].first.dot(n1)-1) < EPSILON*EPSILON) { 
                    best_orientation = n1;
                    break; 
                }
            }
            else {
                break;
            }

        }

        BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(6) << "best:" << best_orientation.transpose() << ", costs:" << results_vector[0].second.field_values();
        std::cout << std::fixed << std::setprecision(6) << "best:" << best_orientation.transpose() << ", costs:" << results_vector[0].second.field_values() << std::endl;

        return best_orientation.cast<double>();
    }

    void preprocess()
    {
        int count_apperance = 0;
        {
            int face_count = mesh->facets_count();
            auto its = mesh->its;
            face_normals = its_face_normals(its);
            areas = Eigen::VectorXf::Zero(face_count);
            is_apperance = Eigen::VectorXf::Zero(face_count);
            normals = Eigen::MatrixXf::Zero(face_count, 3);
            normals_quantize = Eigen::MatrixXf::Zero(face_count, 3);
            for (size_t i = 0; i < face_count; i++)
            {
                float area = its.facet_area(i);
                normals.row(i) = face_normals[i];
                normals_quantize.row(i) = quantize_vec3f(face_normals[i]);
                areas(i) = area;
                is_apperance(i) = (its.get_property(i).type == EnumFaceTypes::eExteriorAppearance);
                count_apperance += (is_apperance(i)==1);
            }
        }

        if (orient_mesh)
            BOOST_LOG_TRIVIAL(debug) <<orient_mesh->name<< ", count_apperance=" << count_apperance;

        // get convex hull statistics
        {
            mesh_convex_hull = mesh->convex_hull_3d();
            //mesh_convex_hull.write_binary("convex_hull_debug.stl");

            int face_count = mesh_convex_hull.facets_count();
            auto its = mesh_convex_hull.its;
            face_count_hull = mesh_convex_hull.facets_count();
            face_normals_hull = its_face_normals(its);
            areas_hull = Eigen::VectorXf::Zero(face_count);
            normals_hull = Eigen::MatrixXf::Zero(face_count_hull, 3);
            normals_hull_quantize = Eigen::MatrixXf::Zero(face_count_hull, 3);
            for (size_t i = 0; i < face_count; i++)
            {
                float area = its.facet_area(i);
                //We cannot use quantized vector here, the accumulated error will result in bad orientations.
                normals_hull.row(i) = face_normals_hull[i];
                normals_hull_quantize.row(i) = quantize_vec3f(face_normals_hull[i]);
                areas_hull(i) = area;
            }
        }
    }

    void area_cumulation(const Eigen::MatrixXf& normals_, const Eigen::VectorXf& areas_, int num_directions = 10)
    {
        std::unordered_map<stl_normal, float, VecHash> alignments;
        // init to 0
        for (size_t i = 0; i < areas_.size(); i++)
            alignments.insert(std::pair(normals_.row(i), 0));
        // cumulate areas
        for (size_t i = 0; i < areas_.size(); i++)
        {
            alignments[normals_.row(i)] += areas_(i);
        }

        typedef std::pair<stl_normal, float> PAIR;
        std::vector<PAIR> align_counts(alignments.begin(), alignments.end());
        sort(align_counts.begin(), align_counts.end(), [](const PAIR& p1, const PAIR& p2) {return p1.second > p2.second; });

        num_directions = std::min((size_t)num_directions, align_counts.size());
        for (size_t i = 0; i < num_directions; i++)
        {
            orientations.push_back(align_counts[i].first);
            //orientations.push_back(its_face_normals(mesh->its)[i]);
            BOOST_LOG_TRIVIAL(debug) << align_counts[i].first.transpose() << ", area: " << align_counts[i].second;
        }
    }
    //This function is to make sure to return the accurate normal rather than quantized normal
    void area_cumulation_accurate( std::vector<Vec3f>& normals_, const Eigen::MatrixXf& quantize_normals_, const Eigen::VectorXf& areas_, int num_directions = 10)
    {
        std::unordered_map<stl_normal, std::pair<std::vector<float>, Vec3f>, VecHash> alignments_;
        Vec3f n1 = { 0, 0, 0 };
        std::vector<float> current_areas = {0, 0};
        // init to 0
        for (size_t i = 0; i < areas_.size(); i++) {
            alignments_.insert(std::pair(quantize_normals_.row(i), std::pair(current_areas, n1)));
        }
        // cumulate areas
        for (size_t i = 0; i < areas_.size(); i++)
        {
            alignments_[quantize_normals_.row(i)].first[1] += areas_(i);
            if (areas_(i) > alignments_[quantize_normals_.row(i)].first[0]){
                alignments_[quantize_normals_.row(i)].second = normals_[i];
                alignments_[quantize_normals_.row(i)].first[0] = areas_(i);
            }
        }

        typedef std::pair<stl_normal, std::pair<std::vector<float>, Vec3f>> PAIR;
        std::vector<PAIR> align_counts(alignments_.begin(), alignments_.end());
        sort(align_counts.begin(), align_counts.end(), [](const PAIR& p1, const PAIR& p2) {return p1.second.first[1] > p2.second.first[1]; });

        num_directions = std::min((size_t)num_directions, align_counts.size());
        for (size_t i = 0; i < num_directions; i++)
        {
            orientations.push_back(align_counts[i].second.second);
            BOOST_LOG_TRIVIAL(debug) << align_counts[i].second.second.transpose() << ", area: " << align_counts[i].second.first[1];
        }
    }
    void add_supplements()
    {
        std::vector<Vec3f> vecs = { {0, 0, -1} ,{0.70710678, 0, -0.70710678},{0, 0.70710678, -0.70710678},
            {-0.70710678, 0, -0.70710678},{0, -0.70710678, -0.70710678},
            {1, 0, 0},{0.70710678, 0.70710678, 0},{0, 1, 0},{-0.70710678, 0.70710678, 0},
            {-1, 0, 0},{-0.70710678, -0.70710678, 0},{0, -1, 0},{0.70710678, -0.70710678, 0},
            {0.70710678, 0, 0.70710678},{0, 0.70710678, 0.70710678},
            {-0.70710678, 0, 0.70710678},{0, -0.70710678, 0.70710678},{0, 0, 1} };
        orientations.insert(orientations.end(), vecs.begin(), vecs.end());
    }

    /// <summary>
    /// remove duplicate orientations
    /// </summary>
    /// <param name="tol">tolerance. default 0.01 =sin(0.57\degree)</param>
    void remove_duplicates(double tol=0.0000001)
    {
        for (auto it = orientations.begin()+1; it < orientations.end(); )
        {
            bool duplicate = false;
            for (auto it_ok = orientations.begin(); it_ok < it; it_ok++)
            {
                if (it_ok->isApprox(*it, tol)) {
                    duplicate = true;
                    break;
                }
            }
            const Vec3f all_zero = { 0,0,0 };
            if (duplicate || it->isApprox(all_zero,tol))
                it = orientations.erase(it);
            else
                it++;
        }
    }

    void project_vertices(Vec3f orientation)
    {
        int face_count = mesh->facets_count();
        auto its = mesh->its;
        z_projected.resize(face_count, 3);
        z_max.resize(face_count, 1);
        z_median.resize(face_count, 1);
        z_mean.resize(face_count, 1);
        for (size_t i = 0; i < face_count; i++)
        {
            float z0 = its.get_vertex(i,0).dot(orientation);
            float z1 = its.get_vertex(i,1).dot(orientation);
            float z2 = its.get_vertex(i,2).dot(orientation);
            z_projected(i, 0) = z0;
            z_projected(i, 1) = z1;
            z_projected(i, 2) = z2;
            z_max(i) = MAX3(z0,z1,z2);
            z_median(i) = MEDIAN3(z0,z1,z2);
            z_mean(i) = (z0 + z1 + z2) / 3;
        }

        z_max_hull.resize(mesh_convex_hull.facets_count(), 1);
        its = mesh_convex_hull.its;
        for (size_t i = 0; i < z_max_hull.rows(); i++)
        {
            float z0 = its.get_vertex(i,0).dot(orientation);
            float z1 = its.get_vertex(i,1).dot(orientation);
            float z2 = its.get_vertex(i,2).dot(orientation);
            z_max_hull(i) = MAX3(z0, z1, z2);
        }
    }

    static Eigen::VectorXi argsort(const Eigen::VectorXf& vec, std::string order="ascend")
    {
        Eigen::VectorXi ind = Eigen::VectorXi::LinSpaced(vec.size(), 0, vec.size() - 1);//[0 1 2 3 ... N-1]
        std::function<bool(int, int)> rule;
        if (order == "ascend") {
            rule = [vec](int i, int j)->bool {
                return vec(i) < vec(j);
                };
            }
        else {
            rule = [vec](int i, int j)->bool {
                return vec(i) > vec(j);
                };
            }
        std::sort(ind.data(), ind.data() + ind.size(), rule);
        return ind;

        //sorted_vec.resize(vec.size());
        //for (int i = 0; i < vec.size(); i++) {
        //    sorted_vec(i) = vec(ind(i));
        //}
    }

    // previously calc_overhang
    CostItems get_features(Vec3f orientation, bool min_volume = true)
    {
        CostItems costs;
        costs.area_total = mesh->bounding_box().area();
        costs.radius = mesh->bounding_box().radius();
        // volume
        costs.volume = mesh->stats().volume > 0 ? mesh->stats().volume : its_volume(mesh->its);

        float total_min_z = z_projected.minCoeff();
        // filter bottom area
        auto bottom_condition = (z_max.array() < total_min_z + this->params.FIRST_LAY_H - EPSILON).eval();
        auto bottom_condition_hull = (z_max_hull.array() < total_min_z + this->params.FIRST_LAY_H - EPSILON).eval();
        auto bottom_condition_2nd  = (z_max.array() < total_min_z + this->params.FIRST_LAY_H / 2.f - EPSILON).eval();
        //The first layer is sliced on half of the first layer height. 
        //The bottom area is measured by accumulating first layer area with the facets area below first layer height.
        //By combining these two factors, we can avoid the wrong orientation of large planar faces while not influence the
        //orientations of complex objects with small bottom areas.
        costs.bottom = bottom_condition.select(areas, 0).sum()*0.5 + bottom_condition_2nd.select(areas, 0).sum();

        // filter overhang
        Eigen::VectorXf normal_projection(normals.rows(), 1);// = this->normals.dot(orientation);
        for (size_t i = 0; i < normals.rows(); i++)
        {
            normal_projection(i) = normals.row(i).dot(orientation);
        }
        auto areas_appearance = areas.cwiseProduct((is_apperance * params.APPERANCE_FACE_SUPP + Eigen::VectorXf::Ones(is_apperance.rows(), is_apperance.cols()))).eval();
        auto overhang_areas = ((normal_projection.array() < params.ASCENT) * (!bottom_condition_2nd)).select(areas_appearance, 0).eval();
        Eigen::MatrixXf inner = normal_projection.array() - params.ASCENT;
        inner = inner.cwiseMin(0).cwiseAbs();
        if (min_volume)
        {
            Eigen::MatrixXf heights = z_mean.array() - total_min_z;
            costs.overhang = (heights.array()* overhang_areas.array()*inner.array()).sum();
        }
        else {
            costs.overhang = overhang_areas.array().cwiseAbs().sum();
        }

        // ATN: the literal surface area (mm^2) that overhangs beyond the user's
        // support-threshold angle and is not resting on the bed. This is what the
        // rewritten objective minimises. Uses real face areas (not the
        // appearance-weighted ones) so it is the true area a slicer would support.
        costs.area_support = ((normal_projection.array() < params.ASCENT) * (!bottom_condition_2nd)).select(areas, 0).sum();

        // ATN: swept support volume (mm^3) under those overhangs, for the print-time
        // objective. Per overhang face: horizontal projected area (area * |n . up|)
        // times its height above the bed (the support column reaches down to the
        // plate, worst case). Summed, this is proportional to the support material
        // a slicer would lay down at this orientation/angle.
        Eigen::ArrayXf supp_col = areas.array() * normal_projection.array().abs()
                                  * (z_mean.array() - total_min_z).max(0.0f);
        costs.support_volume = ((normal_projection.array() < params.ASCENT) * (!bottom_condition_2nd))
                                 .select(supp_col, 0.0f).sum();

        {
            // contour perimeter
#if 1
            // the simple way for contour is even better for faces of small bridges
            costs.contour = 4 * sqrt(costs.bottom);
#else
            float contour = 0;
            int face_count = mesh->facets_count();
            auto its = mesh->its;
            int contour_amout = 0;
            for (size_t i = 0; i < face_count; i++)
            {
                if (bottom_condition(i)) {
                    Eigen::VectorXi index = argsort(z_projected.row(i));
                    stl_vertex line = its.get_vertex(i, index(0)) - its.get_vertex(i, index(1));
                    contour += line.norm();
                    contour_amout++;
                }
            }
            costs.contour += contour + params.CONTOUR_AMOUNT * contour_amout;
#endif
        }

        // bottom of convex hull
        costs.bottom_hull = (bottom_condition_hull).select(areas_hull, 0).sum();

        // low angle faces
        auto normal_projection_abs = normal_projection.cwiseAbs().eval();
        Eigen::MatrixXf laf_areas = ((normal_projection_abs.array() < params.LAF_MAX) * (normal_projection_abs.array() > params.LAF_MIN) * (z_max.array() > total_min_z + params.FIRST_LAY_H)).select(areas, 0);
        costs.area_laf = laf_areas.sum();

        // height to bottom_hull_area ratio
        //float total_max_z = z_projected.maxCoeff();
        //costs.height_to_bottom_hull_ratio = SQ(total_max_z) / (costs.bottom_hull + 1e-7);

        // ATN: bed-fit footprint. The part's footprint is its extent in the
        // plane perpendicular to the (downward) orientation; the part can rotate
        // freely about Z when arranged, so we align the measuring axes with the
        // footprint's principal axes (PCA) to approximate the minimal rectangle.
        // Also penalise height that exceeds the printer's build height (so a
        // part doesn't get stood on end taller than the machine can print).
        costs.height = z_projected.maxCoeff() - total_min_z; // extent along the orientation axis
        float height_excess = 0.f;
        if (params.bed_size_z > 0.f)
            height_excess = std::max(0.f, costs.height - (params.bed_size_z - params.BED_MARGIN));
        costs.footprint_excess = bed_footprint_excess(orientation) + height_excess;

        return costs;
    }

    // Returns mm by which the oriented footprint overflows the bed (0 = fits).
    float bed_footprint_excess(const Vec3f& down)
    {
        if (params.bed_size_x <= 0.f || params.bed_size_y <= 0.f)
            return 0.f;
        const std::vector<stl_vertex>& verts = mesh_convex_hull.its.vertices; // hull bounds the part, far fewer points
        if (verts.size() < 3)
            return 0.f;

        Vec3f o = down.normalized();
        Vec3f ref = std::abs(o.z()) < 0.9f ? Vec3f(0, 0, 1) : Vec3f(1, 0, 0);
        Vec3f u0 = o.cross(ref).normalized();
        Vec3f v0 = o.cross(u0).normalized();

        double sa = 0, sb = 0;
        for (const stl_vertex& p : verts) { sa += p.dot(u0); sb += p.dot(v0); }
        const double n = double(verts.size());
        const double ma = sa / n, mb = sb / n;
        double saa = 0, sab = 0, sbb = 0;
        for (const stl_vertex& p : verts) {
            const double a = p.dot(u0) - ma, b = p.dot(v0) - mb;
            saa += a * a; sab += a * b; sbb += b * b;
        }
        const double theta = 0.5 * std::atan2(2.0 * sab, saa - sbb);
        const Vec3f u = (float)std::cos(theta) * u0 + (float)std::sin(theta) * v0;
        const Vec3f v = (float)-std::sin(theta) * u0 + (float)std::cos(theta) * v0;

        float umin = FLT_MAX, umax = -FLT_MAX, vmin = FLT_MAX, vmax = -FLT_MAX;
        for (const stl_vertex& p : verts) {
            const float a = p.dot(u), b = p.dot(v);
            umin = std::min(umin, a); umax = std::max(umax, a);
            vmin = std::min(vmin, b); vmax = std::max(vmax, b);
        }
        const float flong = std::max(umax - umin, vmax - vmin);
        const float fshort = std::min(umax - umin, vmax - vmin);

        const float blong = std::max(params.bed_size_x, params.bed_size_y) - params.BED_MARGIN;
        const float bshort = std::min(params.bed_size_x, params.bed_size_y) - params.BED_MARGIN;

        return std::max(0.f, flong - blong) + std::max(0.f, fshort - bshort);
    }

    // ATN rewrite: choose the orientation that needs the least support, while
    // staying inside the build volume.
    //
    //   Primary objective  — minimise the part's surface area (mm^2) that
    //                        overhangs beyond the user's support-threshold angle
    //                        (OrientMesh::overhang_angle, e.g. 30 deg) and would
    //                        therefore need support material.
    //   Hard constraint    — the oriented part must fit the printer's X, Y and Z.
    //                        An orientation that overflows the plate or exceeds
    //                        the build height is rejected outright, no matter how
    //                        little support it needs.
    //   Tie-breakers       — among orientations with near-equal support area,
    //                        prefer more bed contact (stability/adhesion) and a
    //                        lower build height (stability, headroom). These are
    //                        deliberately small so they never override the
    //                        primary support-area objective.
    float target_function(CostItems& costs, bool /*min_volume*/)
    {
        const float BOTTOM_REWARD  = 0.01f; // per mm^2 of first-layer bed contact
        const float HEIGHT_PENALTY = 0.10f; // per mm of build height

        float cost;
        if (params.objective == 1) {
            // ATN: minimise print time as an actual time estimate (seconds), so the
            // flat-vs-support trade-off is computed, not weighted by a fudge factor:
            //
            //   T ~= layers * t_layer  +  support_material_volume / Q
            //
            //   layers = build height / layer height            (taller => slower)
            //   support_volume = swept volume under overhangs at the chosen angle,
            //                    times a sparse-support density  (more overhang => slower)
            //   t_layer = fixed per-layer overhead (z-hop + travel + accel ramps)
            //   Q       = volumetric flow rate of the nozzle
            //
            // Only the orientation-dependent terms matter for ranking; the part's own
            // extrusion time is ~constant across orientations and drops out.
            const float t_layer    = 0.8f;   // s of overhead per layer
            const float support_d  = 0.12f;  // sparse support infill fraction
            const float Q          = 8.0f;   // mm^3/s volumetric flow
            const float lh         = params.layer_height > 0.f ? params.layer_height : 0.2f;

            const float t_layers  = (costs.height / lh) * t_layer;
            const float t_support = (costs.support_volume * support_d) / Q;
            cost = t_layers + t_support;
        } else {
            // ATN: minimise support area (default) at the user's overhang angle, with
            // bed-contact reward and a small height penalty as tie-breaks.
            cost = costs.area_support
                 - BOTTOM_REWARD  * costs.bottom
                 + HEIGHT_PENALTY * costs.height;
        }

        // A part with essentially no bed contact will topple or fail to adhere;
        // keep it out of contention even if its support area is low.
        if (costs.bottom < params.BOTTOM_MIN)
            cost += 1000.f;

        // Build-volume fit is a hard constraint: footprint_excess already folds
        // in both the XY overflow and the Z over-height. Reject non-fitting
        // orientations; among them, prefer the least overflow.
        if (costs.footprint_excess > 0.f)
            cost += params.BED_FIT_PENALTY + costs.footprint_excess;

        costs.unprintability = cost;

        return cost;
    }
};

void _orient(OrientMeshs& meshs_,
        const OrientParams           &params,
        std::function<void(unsigned, std::string)> progressfn,
        std::function<bool()>         stopfn)
{
    if (!params.parallel)
    {
        for (size_t i = 0; i != meshs_.size(); ++i) {
            auto& mesh_ = meshs_[i];
            progressfn(i, mesh_.name);
            //auto progressfn_i = [&](unsigned cnt) {progressfn(cnt, "Orienting " + mesh_.name); };
            AutoOrienter orienter(&mesh_, params, /*progressfn_i*/{}, stopfn);
            mesh_.orientation = orienter.process();
            Geometry::rotation_from_two_vectors(mesh_.orientation, { 0,0,1 }, mesh_.axis, mesh_.angle, &mesh_.rotation_matrix);
            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(3) << "v,phi: " << mesh_.axis.transpose() << ", " << mesh_.angle;
            //flush_logs();
        }
    }
    else {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, meshs_.size()), [&meshs_, &params, progressfn, stopfn](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto& mesh_ = meshs_[i];
                progressfn(i, mesh_.name);
                AutoOrienter orienter(&mesh_, params, {}, stopfn);
                mesh_.orientation = orienter.process();
                Geometry::rotation_from_two_vectors(mesh_.orientation, { 0,0,1 }, mesh_.axis, mesh_.angle, &mesh_.rotation_matrix);
                mesh_.euler_angles = Geometry::extract_euler_angles(mesh_.rotation_matrix);
                BOOST_LOG_TRIVIAL(debug) << "rotation_from_two_vectors: " << mesh_.orientation << "; " << mesh_.axis << "; " << mesh_.angle << "; euler: " << mesh_.euler_angles.transpose();
            }});
    }
}

void orient(OrientMeshs &      arrangables,
             const OrientMeshs &excludes,
             const OrientParams &  params)
{

    auto &cfn = params.stopcondition;
    auto &pri = params.progressind;

    _orient(arrangables, params, pri, cfn);

}

void orient(ModelObject* obj)
{
    auto m = obj->mesh();
    AutoOrienter orienter(&m);
    Vec3d orientation = orienter.process();
    Vec3d axis;
    double angle;
    Geometry::rotation_from_two_vectors(orientation, { 0,0,1 }, axis, angle);

    obj->rotate(angle, axis);
    obj->ensure_on_bed();
}

void orient(ModelInstance* instance)
{
    auto m = instance->get_object()->mesh();
    AutoOrienter orienter(&m);
    Vec3d orientation = orienter.process();
    Vec3d axis;
    double angle;
    Matrix3d rotation_matrix;
    Geometry::rotation_from_two_vectors(orientation, { 0,0,1 }, axis, angle, &rotation_matrix);
    instance->rotate(rotation_matrix);
}


} // namespace arr
} // namespace Slic3r
