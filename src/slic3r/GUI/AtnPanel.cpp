#include "AtnPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "SendToFarmDialog.hpp"   // "Connect your farm account" reuses the dialog's browser sign-in
#include "GLCanvas3D.hpp"
#include "Camera.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/calib.hpp"          // Calib_Params / CalibMode for the Calibrate wizard
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <limits>
#include <thread>
#include <wx/base64.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/socket.h>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

// --- ATN Engine auto-spawn ---------------------------------------------------
// The Engine is a local sidecar on 127.0.0.1:8790. In a release install the bundled
// ATNEngine.exe sits next to the slicer; we launch it on demand and pass our PID so it
// self-terminates when the slicer exits. In dev we leave a running `python run.py` alone.
static bool atn_port_open(int port)
{
    wxIPV4address addr;
    addr.Hostname("127.0.0.1");
    addr.Service(port);
    wxSocketClient sock(wxSOCKET_BLOCK);
    sock.SetTimeout(1);
    const bool ok = sock.Connect(addr, true);
    sock.Close();
    return ok;
}

static void atn_ensure_engine()
{
    if (atn_port_open(8790))
        return;   // already serving (dev `python run.py`, or a prior instance)
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    const wxString engine = exe.GetPathWithSep() + "ATNEngine.exe";
    if (!wxFileExists(engine))
        return;   // plain dev build with no bundled engine -> user runs python run.py
    wxSetEnv("ATN_PARENT_PID", wxString::Format("%lu", (unsigned long) wxGetProcessId()));
    wxExecute("\"" + engine + "\"", wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
    BOOST_LOG_TRIVIAL(info) << "AtnPanel: launched bundled ATN Engine sidecar";
}

AtnPanel::AtnPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    atn_ensure_engine();   // start the local Engine sidecar if it isn't already running

    // URL priority: ATN_PANEL_URL env var (dev override) > app config > hosted assistant.
    std::string url;
    if (const char* env_url = std::getenv("ATN_PANEL_URL"); env_url != nullptr && *env_url != 0)
        url = env_url;
    if (url.empty())
        url = wxGetApp().app_config->get("atn_panel_url");
    if (url.empty())
        url = "https://askthenozzle.com/slicer/assistant";

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    m_browser = WebView::CreateWebView(this, from_u8(url));
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "AtnPanel: could not create webview";
        return;
    }
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &AtnPanel::on_script_message, this);

    // Open external links (target="_blank", e.g. the Upgrade / subscribe link) in
    // the user's default browser instead of navigating the panel away from the
    // assistant.
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& evt) {
        if (!evt.GetURL().empty())
            wxLaunchDefaultBrowser(evt.GetURL());
    });
}

void AtnPanel::load_url(const wxString& url)
{
    if (m_browser != nullptr)
        m_browser->LoadURL(url);
}

void AtnPanel::set_mode(const std::string& mode)
{
    json msg;
    msg["command"]      = "atn_mode";
    msg["data"]["mode"] = mode;
    send_to_page(msg.dump());
}

void AtnPanel::on_slice_complete()
{
    // ATN: the sphere risk markers have been retired in favour of a g-code Thermal
    // view; nothing to surface here now beyond telling the page a slice finished.
    send_fea_mesh();   // stage the plate's model mesh for the hybrid FEA view
    json msg;
    msg["command"] = "atn_slice_complete";
    send_to_page(msg.dump());
}

void AtnPanel::send_farm_status()
{
    // Read the persisted cloud-farm session (written by SendToFarmDialog's sign-in). The panel
    // uses this to show "Connect your farm account" vs "Farm: <account>".
    std::string token, account;
    if (auto* ac = wxGetApp().app_config) {
        token   = ac->get("farm", "token");
        account = ac->get("farm", "account");
    }
    json msg;
    msg["command"] = "atn_farm_status";
    msg["data"]["connected"] = !token.empty();
    msg["data"]["account"]   = account;
    send_to_page(msg.dump());
}

void AtnPanel::send_fea_mesh()
{
    // Feed the ATN Engine the current plate's MODEL MESH for the hybrid FEA view (part = real
    // mesh, supports = g-code voxels reconstructed engine-side). Pure transport, no proprietary
    // logic: merge the plate's printable instances in world/model coords, decimate, and POST
    // {verts,faces,prealigned:false} to the local Engine, which registers it to the part field.
    // Best-effort -- if the Engine isn't up/licensed it simply no-ops and /warp/view falls back
    // to the voxel-only reconstruction.
    if (!atn_port_open(8790))
        return;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return;

    // Merge the current plate's printable object instances into one mesh (world/model coords).
    TriangleMesh merged;
    Model& model = wxGetApp().model();
    for (int obj_idx = 0; obj_idx < static_cast<int>(model.objects.size()); ++obj_idx) {
        ModelObject* mo = model.objects[obj_idx];
        TriangleMesh obj_mesh;
        for (const ModelVolume* v : mo->volumes)
            if (v->is_model_part()) {
                TriangleMesh vm(v->mesh());
                vm.transform(v->get_matrix(), true);
                obj_mesh.merge(vm);
            }
        if (obj_mesh.empty())
            continue;
        for (int inst_idx = 0; inst_idx < static_cast<int>(mo->instances.size()); ++inst_idx) {
            const ModelInstance* inst = mo->instances[inst_idx];
            if (!inst->printable || !plate->contain_instance(obj_idx, inst_idx))
                continue;
            TriangleMesh m = obj_mesh;
            m.transform(inst->get_matrix(), true);
            merged.merge(m);
        }
    }
    if (merged.its.vertices.empty() || merged.its.indices.empty())
        return;

    // g-code/print frame = world - plate origin (what the Print subtracts at export; see
    // PartPlate set_plate_origin + local-coord calc). Sending in this frame registers the mesh
    // EXACTLY onto the field + supports -- no bbox-align guessing (which the airbox's thin,
    // sub-voxel ears would skew anyway).
    const Vec3f plate_origin = plate->get_origin().cast<float>();

    // Decimate + serialise + POST off the main thread. The model is stable post-slice and we take
    // a value copy of the geometry, so the worker never touches live model state.
    indexed_triangle_set its = std::move(merged.its);
    std::thread([its = std::move(its), plate_origin]() mutable {
        try {
            constexpr uint32_t kTargetTris = 150000;
            if (its.indices.size() > kTargetTris)
                its_quadric_edge_collapse(its, kTargetTris);
            for (Vec3f& v : its.vertices)
                v -= plate_origin;   // world/model -> g-code/print frame

            nlohmann::json body;
            nlohmann::json& verts = body["verts"] = nlohmann::json::array();
            for (const Vec3f& v : its.vertices)
                verts.push_back({ v.x(), v.y(), v.z() });
            nlohmann::json& faces = body["faces"] = nlohmann::json::array();
            for (const auto& f : its.indices)
                faces.push_back({ f[0], f[1], f[2] });
            body["prealigned"] = true;

            Http::post("http://127.0.0.1:8790/warp/mesh")
                .header("Content-Type", "application/json")
                .set_post_body(body.dump())
                .on_complete([](std::string, unsigned status) {
                    BOOST_LOG_TRIVIAL(info) << "atn: FEA mesh sent to Engine, status=" << status;
                })
                .on_error([](std::string, std::string err, unsigned status) {
                    BOOST_LOG_TRIVIAL(warning) << "atn: FEA mesh send failed, status=" << status << " err=" << err;
                })
                .perform_sync();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "atn: send_fea_mesh worker failed: " << e.what();
        }
    }).detach();
}

void AtnPanel::on_sent_to_farm()
{
    // ATN: a job was dispatched to the farm -> tell the page so the learning diary records it
    // (only prints and farm-sent jobs are stored, not every slice).
    json msg;
    msg["command"] = "atn_sent_to_farm";
    send_to_page(msg.dump());
}

void AtnPanel::send_to_page(const std::string& json_payload)
{
    if (m_browser != nullptr)
        WebView::RunScript(m_browser, wxString::Format("window.postMessage(%s)", from_u8(json_payload)));
}

void AtnPanel::on_script_message(wxWebViewEvent& evt)
{
    const std::string message = evt.GetString().ToUTF8().data();
    json root = json::parse(message, nullptr, false);
    if (root.is_discarded() || !root.contains("command")) {
        // Not ours - forward to the generic handler used by other web pages.
        wxGetApp().handle_web_request(message);
        return;
    }
    const std::string command = root["command"].get<std::string>();

    try {
        if (command == "atn_get_context") {
            json reply;
            reply["command"] = "atn_context";
            reply["data"]    = json::parse(build_context_json());
            send_to_page(reply.dump());
        } else if (command == "atn_highlight_setting") {
            handle_highlight_setting(root["data"]["key"].get<std::string>());
        } else if (command == "atn_highlight_tool") {
            handle_highlight_tool(root["data"]["tool"].get<std::string>());
        } else if (command == "atn_set_setting") {
            handle_set_setting(root["data"]["key"].get<std::string>(), root["data"]["value"].get<std::string>());
        } else if (command == "atn_request_preflight") {
            handle_request_preflight();
        } else if (command == "atn_reslice") {
            // Re-slice on demand (e.g. the panel's "re-slice & re-check" button
            // after the user applied a recommendation). A successful slice fires
            // on_slice_complete(), which tells the page to re-run pre-flight.
            if (auto* p = wxGetApp().plater()) {
                p->reslice();
                // No-op guard: if the user reverted a setting the plate is already up-to-date, so
                // reslice() runs nothing and fires no SLICING_COMPLETED -> the page would sit on the
                // previous slice's result. Once the event loop has settled, if no slice is actually
                // running, tell the page anyway so it refreshes the CURRENT slice. (A real slice IS
                // running here, so we skip -- its own completion fires on_slice_complete().)
                wxGetApp().CallAfter([this]() {
                    if (auto* pl = wxGetApp().plater(); pl && !pl->is_background_process_slicing())
                        on_slice_complete();
                });
            }
        } else if (command == "atn_calibrate_start") {
            // ATN Calibrate wizard: generate + slice one of the built-in calibration
            // test plates (temp / flow / PA / retraction / max-vol), mirroring the
            // Calibration-menu dialogs, then reslice() so the user gets a ready-to-
            // print plate. Like the menu, calib_* wipes the current plate (new_project).
            Plater* p = wxGetApp().plater();
            if (p != nullptr && root.contains("data")) {
                const json&  d    = root["data"];
                const std::string test = d.value("test", std::string());
                Calib_Params params;
                params.print_numbers = false;   // Calib_Params leaves this uninitialised
                bool ok = true;
                if (test == "temp") {
                    params.mode  = CalibMode::Calib_Temp_Tower;
                    params.start = d.value("start", 230.0);
                    params.end   = d.value("end",   190.0);
                    params.step  = d.value("step",    5.0);
                    p->calib_temp(params);
                } else if (test == "flow") {
                    p->calib_flowrate(true, 1);   // "YOLO (Recommended)" linear flow (default pattern)
                } else if (test == "pa") {
                    params.mode  = CalibMode::Calib_PA_Tower;
                    params.start = d.value("start", 0.0);
                    params.end   = d.value("end",   0.1);
                    params.step  = d.value("step",  0.002);
                    p->calib_pa(params);
                } else if (test == "retract") {
                    params.mode  = CalibMode::Calib_Retraction_tower;
                    params.start = d.value("start", 0.0);
                    params.end   = d.value("end",   2.0);
                    params.step  = d.value("step",  0.1);
                    p->calib_retraction(params);
                } else if (test == "maxvol") {
                    params.mode  = CalibMode::Calib_Vol_speed_Tower;
                    params.start = d.value("start", 5.0);
                    params.end   = d.value("end",  20.0);
                    params.step  = d.value("step",  0.5);
                    p->calib_max_vol_speed(params);
                } else {
                    ok = false;
                }
                if (ok)
                    p->reslice();   // slice the calibration plate so it's ready to print
                json reply;
                reply["command"]      = "atn_calibrate_started";
                reply["data"]["test"] = test;
                reply["data"]["ok"]   = ok;
                send_to_page(reply.dump());
            }
        } else if (command == "atn_farm_signin") {
            // Panel "Connect your farm account": open the Send-to-Farm dialog so the user can sign
            // in BEFORE the Send-to-Farm button is visible (it's gated on farm config -> chicken-and-
            // egg for a brand-new cloud customer). The dialog's own browser sign-in saves the token;
            // once saved, atn_farm_available() is true and the button appears on the next print-
            // dropdown open. Reuses everything, no plate required to open.
            SendToFarmDialog(wxGetApp().mainframe).ShowModal();
            send_farm_status();   // reflect the (possibly new) connection state in the panel
        } else if (command == "atn_get_farm_status") {
            send_farm_status();
        } else if (command == "atn_capture_model") {
            handle_capture_model();
        } else if (command == "atn_apply_optimized") {
            handle_apply_optimized(root["data"]["b64"].get<std::string>(),
                                   (size_t)root["data"]["raw_size"].get<long long>());
        } else if (command == "atn_show_risk") {
            // ATN: let the assistant page toggle the preview risk layers independently.
            // {"support":bool} and/or {"thermal":bool} set each layer; legacy {"on":bool}
            // toggles both together.
            Plater*     p = wxGetApp().plater();
            GLCanvas3D* c = p ? p->get_preview_canvas3D() : nullptr;
            if (c != nullptr && root.contains("data")) {
                const json& d = root["data"];
                if (d.contains("support")) c->set_show_atn_risk_markers(d["support"].get<bool>());
                if (d.contains("thermal")) c->set_show_atn_thermal_markers(d["thermal"].get<bool>());
                if (d.contains("on")) {
                    const bool on = d["on"].get<bool>();
                    c->set_show_atn_risk_markers(on);
                    c->set_show_atn_thermal_markers(on);
                }
            }
        } else if (command == "atn_warp_fea") {
            // ATN: reduced-FEA support-failure markers from the panel's /warp result —
            // green->red spheres on the supports the Engine predicts will fail.
            Plater*     p = wxGetApp().plater();
            GLCanvas3D* c = p ? p->get_preview_canvas3D() : nullptr;
            if (c != nullptr && root.contains("data") && root["data"].contains("markers")) {
                std::vector<Vec3d> pos;
                std::vector<float> uts;
                for (const auto& m : root["data"]["markers"]) {
                    pos.emplace_back(m.value("x", 0.0), m.value("y", 0.0), m.value("z", 0.0));
                    uts.push_back(float(m.value("util", 0.0)));
                }
                c->set_atn_fea_markers(pos, uts);
            }
        } else {
            // Unknown to us - let the app-wide handler have a go.
            wxGetApp().handle_web_request(message);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AtnPanel: error handling command " << command << ": " << e.what();
        json reply;
        reply["command"]         = "atn_error";
        reply["data"]["message"] = e.what();
        send_to_page(reply.dump());
    }
}

// ATN: a structured "risk fingerprint" of the current slice. The server-side
// pre-flight + learning loop want features, not just raw gcode: where the part
// overhangs/curls, how fragile the connections are, the bed footprint vs height,
// and the material/tool budget. Geometry (overhang/curl/sharp-tail/cantilever)
// comes from the sliced libslic3r layers; the volume/time/tool figures come from
// the estimated statistics. Everything is read-only and safe to call pre-slice
// (returns {"sliced": false}).
static json compute_fingerprint()
{
    json fp = json::object();

    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) { fp["sliced"] = false; return fp; }

    const Print& print = plater->fff_print();
    const auto& objects = print.objects();

    bool sliced = false;
    for (const PrintObject* po : objects)
        if (!po->layers().empty()) { sliced = true; break; }
    fp["sliced"] = sliced;
    if (!sliced) return fp;

    const double sf2 = SCALING_FACTOR * SCALING_FACTOR; // scaled area -> mm^2

    double total_overhang_area = 0.0, max_layer_overhang_area = 0.0;
    double max_curl_height = 0.0, max_sharp_tail_height = 0.0;
    int    sharp_tail_count = 0, cantilever_count = 0;
    double first_layer_area = 0.0;
    int    first_layer_islands = 0;
    double max_z = 0.0, tallest_aspect = 0.0;
    double tallest_support = 0.0, min_branch_w = 1e9;
    bool   any_support = false;

    for (const PrintObject* po : objects) {
        const auto& layers = po->layers();
        if (layers.empty()) continue;

        const Layer* first = layers.front();
        for (const ExPolygon& isl : first->lslices)
            first_layer_area += isl.area() * sf2;
        first_layer_islands += (int) first->lslices.size();

        double obj_min_xy = 0.0;
        if (!first->lslices_bboxes.empty()) {
            BoundingBox bb = first->lslices_bboxes.front();
            for (const BoundingBox& b : first->lslices_bboxes) bb.merge(b);
            obj_min_xy = std::min(unscale<double>(bb.size().x()), unscale<double>(bb.size().y()));
        }

        double obj_max_z = 0.0;
        for (const Layer* l : layers) {
            obj_max_z = std::max(obj_max_z, (double) l->print_z);
            for (const ExPolygon& ov : l->loverhangs) {
                const double a = ov.area() * sf2;
                total_overhang_area += a;
                max_layer_overhang_area = std::max(max_layer_overhang_area, a);
            }
            for (const CurledLine& cl : l->curled_lines)
                max_curl_height = std::max(max_curl_height, (double) cl.curled_height);
            sharp_tail_count += (int) l->sharp_tails.size();
            for (float sh : l->sharp_tails_height)
                max_sharp_tail_height = std::max(max_sharp_tail_height, (double) sh);
            cantilever_count += (int) l->cantilevers.size();
        }
        max_z = std::max(max_z, obj_max_z);
        if (obj_min_xy > 1e-6)
            tallest_aspect = std::max(tallest_aspect, obj_max_z / obj_min_xy);

        // Support-structure metrics for the preflight support-snap learning: how tall
        // the supports get and how thin the thinnest branch (above the base) becomes.
        // The learning loop pairs these with the material + wall count (already in the
        // config block) and the user's real snap outcomes to build a per-material rule.
        if (po->support_layer_count() > 0) {
            any_support = true;
            for (const SupportLayer* sl : po->support_layers()) {
                tallest_support = std::max(tallest_support, double(sl->print_z));
                if (sl->print_z < 3.0) continue; // ignore the thick base; necks higher up snap
                const ExPolygons& islands = !sl->support_islands.empty() ? sl->support_islands : sl->base_areas;
                for (const ExPolygon& isl : islands) {
                    const double a = isl.area() * sf2;
                    if (a < 0.2) continue;
                    min_branch_w = std::min(min_branch_w, std::sqrt(a));
                }
            }
        }
    }

    json g = json::object();
    g["max_z_mm"]                    = max_z;
    g["first_layer_area_mm2"]        = first_layer_area;
    g["first_layer_islands"]         = first_layer_islands;
    g["total_overhang_area_mm2"]     = total_overhang_area;
    g["max_layer_overhang_area_mm2"] = max_layer_overhang_area;
    g["sharp_tail_count"]            = sharp_tail_count;
    g["max_sharp_tail_height_mm"]    = max_sharp_tail_height;
    g["cantilever_count"]            = cantilever_count;
    g["max_curl_height_mm"]          = max_curl_height;
    g["tallest_aspect_ratio"]        = tallest_aspect;
    fp["geometry"] = g;

    json sup = json::object();
    sup["present"] = any_support;
    if (any_support) {
        sup["tallest_mm"] = tallest_support;
        if (min_branch_w < 1e8) {
            sup["min_branch_width_mm"] = min_branch_w;
            sup["slenderness"]         = (min_branch_w > 0.1) ? (tallest_support / min_branch_w) : 0.0;
        }
    }
    fp["supports"] = sup;

    const PrintStatistics& ps = print.print_statistics();
    fp["total_weight_g"]   = ps.total_weight;
    fp["total_volume_mm3"] = ps.total_extruded_volume;
    fp["toolchanges"]      = ps.total_toolchanges;

    PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    GCodeProcessorResult* res = plate ? plate->get_slice_result() : nullptr;
    if (res != nullptr) {
        const PrintEstimatedStatistics& es = res->print_statistics;
        fp["time_normal_s"]      = es.modes[(size_t) PrintEstimatedStatistics::ETimeMode::Normal].time;
        fp["filament_changes"]   = es.total_filament_changes;
        fp["extruder_changes"]   = es.total_extruder_changes;
        double support_vol = 0.0;
        for (const auto& kv : es.support_volumes_per_extruder) support_vol += kv.second;
        fp["support_volume_mm3"] = support_vol;
        fp["travel_distance_mm"] = es.total_travel_distance;

        json vpe = json::object();
        for (const auto& kv : es.total_volumes_per_extruder)
            vpe[std::to_string(kv.first)] = kv.second;
        fp["volume_per_extruder"] = vpe;

        json vpr = json::object();
        double bridge_len = 0.0;
        for (const auto& kv : es.used_filaments_per_role) {
            std::string role = ExtrusionEntity::role_to_string(kv.first);
            json one; one["length_mm"] = kv.second.first; one["volume_mm3"] = kv.second.second;
            vpr[role] = one;
            std::string lower = role;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)::tolower(c); });
            if (lower.find("bridge") != std::string::npos) bridge_len += kv.second.first;
        }
        fp["volume_per_role"]  = vpr;
        fp["bridge_length_mm"] = bridge_len;
    }

    return fp;
}

std::string AtnPanel::build_context_json() const
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;

    json ctx;
    ctx["app"]            = std::string(SLIC3R_APP_NAME) + " " + std::string(SLIC3R_VERSION);
    ctx["printer_preset"]  = bundle.printers.get_edited_preset().name;
    ctx["print_preset"]    = bundle.prints.get_edited_preset().name;
    ctx["filament_presets"] = bundle.filament_presets;

    // Full effective configuration, values serialized the same way profiles store them.
    DynamicPrintConfig full = bundle.full_config();
    json cfg = json::object();
    for (const std::string& key : full.keys())
        cfg[key] = full.opt_serialize(key);
    ctx["config"] = cfg;

    // Which options the user changed relative to the selected presets.
    ctx["modified"]["print"]    = bundle.prints.current_dirty_options();
    ctx["modified"]["filament"] = bundle.filaments.current_dirty_options();
    ctx["modified"]["printer"]  = bundle.printers.current_dirty_options();

    // Objects on the plate.
    json objects = json::array();
    for (const ModelObject* mo : wxGetApp().plater()->model().objects) {
        json o;
        o["name"]            = mo->name;
        const BoundingBoxf3 bb = mo->bounding_box_exact();
        o["size_mm"]         = { bb.size().x(), bb.size().y(), bb.size().z() };
        objects.push_back(o);
    }
    ctx["objects"] = objects;

    // Results of the floating extrusion detection, if a slice has run.
    const Print& print = wxGetApp().plater()->fff_print();
    json floating = json::array();
    for (const PrintObject* po : print.objects()) {
        for (const SupportSpotsGenerator::FloatingExtrusionSpot& spot : po->floating_extrusion_spots()) {
            json s;
            s["position"]        = { spot.position.x(), spot.position.y(), spot.position.z() };
            s["unsupported_len"] = spot.unsupported_len;
            floating.push_back(s);
        }
    }
    ctx["floating_extrusions"] = floating;

    // ATN: structured risk fingerprint for the server pre-flight + learning loop.
    ctx["fingerprint"] = compute_fingerprint();

    return ctx.dump();
}

void AtnPanel::handle_highlight_setting(const std::string& key)
{
    // Figure out which preset type owns this key.
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    Preset::Type type = Preset::TYPE_PRINT;
    if (bundle.prints.get_edited_preset().config.has(key))
        type = Preset::TYPE_PRINT;
    else if (bundle.filaments.get_edited_preset().config.has(key))
        type = Preset::TYPE_FILAMENT;
    else if (bundle.printers.get_edited_preset().config.has(key))
        type = Preset::TYPE_PRINTER;
    else {
        json reply;
        reply["command"]         = "atn_error";
        reply["data"]["message"] = "unknown setting: " + key;
        send_to_page(reply.dump());
        return;
    }

    // Bring the editor into view, then jump and blink.
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    wxGetApp().sidebar().jump_to_option(key, type, L"");
}

// ATN: let the assistant point the user at an actual tool/button (blinking arrow),
// so it never has to describe a screen position it might get wrong. Maps a stable
// friendly key to either a left-toolbar gizmo (Cut, Move, …) or a top-toolbar item
// (Arrange, Auto-orient, …) and triggers Orca's built-in highlighter.
void AtnPanel::handle_highlight_tool(const std::string& tool)
{
    // {friendly key -> {is_gizmo, internal name}}. Gizmo names are the icon
    // basename (theme suffix handled below); toolbar names are the item ids.
    static const std::unordered_map<std::string, std::pair<bool, std::string>> kTools = {
        {"move",            {true,  "toolbar_move"}},
        {"rotate",          {true,  "toolbar_rotate"}},
        {"scale",           {true,  "toolbar_scale"}},
        {"place_on_face",   {true,  "toolbar_flatten"}},
        {"flatten",         {true,  "toolbar_flatten"}},
        {"cut",             {true,  "toolbar_cut"}},
        {"mesh_boolean",    {true,  "toolbar_meshboolean"}},
        {"boolean",         {true,  "toolbar_meshboolean"}},
        {"supports",        {true,  "toolbar_support"}},
        {"support_painter", {true,  "toolbar_support"}},
        {"seam",            {true,  "toolbar_seam"}},
        {"fuzzy_skin",      {true,  "toolbar_fuzzy_skin_paint"}},
        {"color_paint",     {true,  "mmu_segmentation"}},
        {"mmu",             {true,  "mmu_segmentation"}},
        {"text",            {true,  "toolbar_text"}},
        {"measure",         {true,  "toolbar_measure"}},
        {"assembly",        {true,  "toolbar_assembly"}},
        {"add",             {false, "add"}},
        {"import",          {false, "add"}},
        {"add_plate",       {false, "addplate"}},
        {"auto_orient",     {false, "orient"}},
        {"orient",          {false, "orient"}},
        {"arrange",         {false, "arrange"}},
        {"split_objects",   {false, "splitobjects"}},
        {"split_parts",     {false, "splitvolumes"}},
        {"variable_layer_height", {false, "layersediting"}},
    };

    json reply;
    reply["command"]     = "atn_tool_highlighted";
    reply["data"]["tool"] = tool;

    const auto it = kTools.find(tool);
    GLCanvas3D* canvas = wxGetApp().plater() ? wxGetApp().plater()->get_view3D_canvas3D() : nullptr;
    if (it == kTools.end() || canvas == nullptr) {
        reply["data"]["ok"] = false;
        send_to_page(reply.dump());
        return;
    }

    // Bring the 3D editor into view so the blinking arrow is visible.
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (it->second.first) {
        // Gizmo: the icon basename carries a "_dark" suffix in dark theme; try both,
        // the non-matching one is a harmless no-op.
        canvas->highlight_gizmo(it->second.second);
        canvas->highlight_gizmo(it->second.second + "_dark");
    } else {
        canvas->highlight_toolbar_item(it->second.second);
    }
    reply["data"]["ok"] = true;
    send_to_page(reply.dump());
}

// Returns the edited config that owns this key, or nullptr if no preset defines it.
static const DynamicPrintConfig* config_for_key(const PresetBundle& bundle, const std::string& key, Preset::Type& type_out)
{
    if (bundle.prints.get_edited_preset().config.has(key)) {
        type_out = Preset::TYPE_PRINT;
        return &bundle.prints.get_edited_preset().config;
    }
    if (bundle.filaments.get_edited_preset().config.has(key)) {
        type_out = Preset::TYPE_FILAMENT;
        return &bundle.filaments.get_edited_preset().config;
    }
    if (bundle.printers.get_edited_preset().config.has(key)) {
        type_out = Preset::TYPE_PRINTER;
        return &bundle.printers.get_edited_preset().config;
    }
    return nullptr;
}

void AtnPanel::handle_set_setting(const std::string& key, const std::string& value)
{
    json reply;
    reply["command"]     = "atn_set_setting_result";
    reply["data"]["key"] = key;

    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    Preset::Type type;
    const DynamicPrintConfig* cfg = config_for_key(bundle, key, type);
    if (cfg == nullptr) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "unknown setting: " + key;
        send_to_page(reply.dump());
        return;
    }

    DynamicPrintConfig delta;
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
    try {
        delta.set_deserialize(key, value, ctx);
    } catch (const std::exception& e) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = std::string("invalid value: ") + e.what();
        send_to_page(reply.dump());
        return;
    }

    const std::string before = cfg->opt_serialize(key);

    // Apply through the Tab so the field updates, the preset is marked
    // modified, and slicing state is invalidated - same as a manual edit.
    wxGetApp().get_tab(type)->load_config(delta);

    const std::string after   = cfg->opt_serialize(key);
    const bool        changed = (before != after);

    // Only pull the view to the option when we actually changed it, so applying
    // a batch of recommendations where most are already correct doesn't yank the
    // editor around (and doesn't reset the highlighter off the one that changed).
    if (changed) {
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
        wxGetApp().sidebar().jump_to_option(key, type, L"");
    }

    reply["data"]["ok"]       = true;
    reply["data"]["changed"]  = changed;
    reply["data"]["value"]    = after;
    reply["data"]["previous"] = before;
    send_to_page(reply.dump());
}

void AtnPanel::handle_request_preflight()
{
    json reply;
    reply["command"] = "atn_gcode";

    // Use the presence of a sliced gcode file as the source of truth rather than
    // the is_slice_result_valid() flag, which a stray state change can flip even
    // though a perfectly good gcode is still on disk.
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    std::string path;
    if (plate != nullptr) {
        GCodeProcessorResult* res = plate->get_slice_result();
        if (res != nullptr && !res->filename.empty())
            path = res->filename;
        if (path.empty() || !boost::filesystem::exists(path))
            path = plate->get_tmp_gcode_path();
    }
    if (plate == nullptr || path.empty() || !boost::filesystem::exists(path)) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "No sliced G-code found for this plate - slice it (or re-slice if you changed a setting) and try again.";
        send_to_page(reply.dump());
        return;
    }

    // Generous cap: the gcode is deflate-compressed before crossing the bridge
    // (gcode is ~10x compressible), so even large manifolds stay small. The
    // server independently skips its geometry walk above ~50 MB decompressed.
    // Ceiling is really the webview bridge (the compressed gcode rides in one
    // postMessage), not this number — past ~500 MB raw, use the web tool.
    const std::uintmax_t size = boost::filesystem::file_size(path);
    if (size > 500ull * 1024 * 1024) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "This G-code is too large for the in-app pre-flight; use the askthenozzle.com tool instead.";
        send_to_page(reply.dump());
        return;
    }

    boost::nowide::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();

    // Deflate (zlib format) so DecompressionStream('deflate') in the panel can
    // inflate it natively, then upload the raw gcode to the shared preflight.
    mz_ulong bound = mz_compressBound((mz_ulong)bytes.size());
    std::vector<unsigned char> comp(bound);
    const int rc = mz_compress2(comp.data(), &bound,
                                reinterpret_cast<const unsigned char*>(bytes.data()),
                                (mz_ulong)bytes.size(), MZ_BEST_SPEED);
    reply["data"]["ok"]   = true;
    reply["data"]["name"] = boost::filesystem::path(path).filename().string();
    if (rc == MZ_OK) {
        comp.resize(bound);
        reply["data"]["enc"] = "deflate";
        reply["data"]["b64"] = wxBase64Encode(comp.data(), comp.size()).ToStdString();
    } else {
        // Compression failed for some reason - fall back to raw bytes.
        reply["data"]["enc"] = "raw";
        reply["data"]["b64"] = wxBase64Encode(bytes.data(), bytes.size()).ToStdString();
    }
    send_to_page(reply.dump());
}

void AtnPanel::handle_apply_optimized(const std::string& b64, size_t raw_size)
{
    json reply;
    reply["command"] = "atn_optimized_applied";

    Plater*    plater = wxGetApp().plater();
    PartPlate* plate  = plater ? plater->get_partplate_list().get_curr_plate() : nullptr;
    if (plate == nullptr || !plate->is_slice_result_valid()) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "No valid sliced plate to apply to.";
        send_to_page(reply.dump());
        return;
    }

    // Decode + inflate (zlib) the optimized gcode the server sent back.
    wxMemoryBuffer comp = wxBase64Decode(b64);
    std::vector<unsigned char> raw(raw_size > 0 ? raw_size : 1);
    mz_ulong out_len = (mz_ulong)raw.size();
    const int rc = mz_uncompress(raw.data(), &out_len,
                                 reinterpret_cast<const unsigned char*>(comp.GetData()),
                                 (mz_ulong)comp.GetDataLen());
    if (rc != MZ_OK) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "Could not decompress the optimized gcode.";
        send_to_page(reply.dump());
        return;
    }
    raw.resize(out_len);

    // The plate's own sliced g-code file is held OPEN (locked) by the loaded preview, so overwriting
    // it silently fails on Windows -- boost::nowide::ofstream sets failbit rather than throwing, so
    // the old code "succeeded" while writing nothing, and the reprocess re-read the ORIGINAL (Apply
    // looked like a no-op). Write the corrected g-code to a FRESH sibling file instead, verify it
    // landed, and point the plate at that. A unique name per apply avoids re-hitting the lock on the
    // file we just wrote (the preview now holds THAT one).
    static int s_opt_seq = 0;
    boost::filesystem::path base(plate->get_tmp_gcode_path());
    boost::filesystem::path newpath =
        base.parent_path() / (base.stem().string() + ".atn_opt" + std::to_string(++s_opt_seq) + ".gcode");
    {
        boost::nowide::ofstream out(newpath.string(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(raw.data()), raw.size());
        out.flush();
        const bool wrote = out.good();
        out.close();
        if (!wrote) {
            reply["data"]["ok"]      = false;
            reply["data"]["message"] = "Could not write the optimised g-code to disk.";
            send_to_page(reply.dump());
            return;
        }
    }

    // Read the estimated print time before/after the reprocess so the panel can
    // confirm the change definitively, regardless of which slicer field updates.
    auto normal_time_s = [&]() -> float {
        GCodeProcessorResult* r = plate->get_slice_result();
        return r ? r->print_statistics.modes[
                       static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time
                 : 0.0f;
    };
    const float before_s = normal_time_s();
    const bool  ok       = plater->apply_optimized_gcode(newpath.string());
    const float after_s  = normal_time_s();

    reply["data"]["ok"]       = ok;
    reply["data"]["before_s"] = before_s;
    reply["data"]["after_s"]  = after_s;
    reply["data"]["message"]  = ok ? "Applied to the preview and print output." :
                                     "Wrote the gcode but the preview reload failed.";
    send_to_page(reply.dump());
}

void AtnPanel::handle_capture_model()
{
    json reply;
    reply["command"] = "atn_model_images";

    Plater*     plater = wxGetApp().plater();
    GLCanvas3D* canvas = plater ? plater->get_view3D_canvas3D() : nullptr;
    if (canvas == nullptr || wxGetApp().model().objects.empty()) {
        reply["data"]["ok"] = false;
        send_to_page(reply.dump());
        return;
    }

    PartPlateList& plates = plater->get_partplate_list();
    // sizes, printable_only, parts_only, show_bed, transparent_background, plate_id
    ThumbnailsParams params{ Vec2ds{}, false, false, true, false, plates.get_curr_plate_index() };
    const unsigned int W = 512, H = 512;

    json images = json::array();
    auto capture = [&](Camera::ViewAngleType view) {
        ThumbnailData data;
        canvas->render_thumbnail(data, W, H, params, Camera::EType::Ortho, view, false, false);
        if (!data.is_valid())
            return;
        auto png = Slic3r::GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
        if (png && png->data != nullptr && png->size > 0)
            images.push_back(wxBase64Encode(png->data, png->size).ToStdString());
    };
    capture(Camera::ViewAngleType::Iso);
    capture(Camera::ViewAngleType::Top_Plate);

    reply["data"]["ok"]     = !images.empty();
    reply["data"]["images"] = images;
    send_to_page(reply.dump());
}

} // namespace GUI
} // namespace Slic3r
