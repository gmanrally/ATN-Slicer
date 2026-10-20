#include "QidiPrinterLink.hpp"

#include <algorithm>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"

using json = nlohmann::json;

namespace Slic3r {

// ATN: the four send-dialog options and where a Qidi X-Max 4 / X-Plus 4 class firmware reads them
// (verified against the printer's own macro files, 2026-10-20):
//   Timelapse          -> Moonraker timelapse component, POST /machine/timelapse/settings {"enabled": b}
//   Auto Bed Leveling  -> gcode_macro _km_globals.bedmesh_before_print, read by the `g29` macro the
//                         start G-code calls: True = BED_MESH_CALIBRATE (adaptive), False = load the saved mesh
//   Enable Box         -> save_variables enable_box, read by the box start/flush macros (only meaningful
//                         with box_count >= 1; the firmware forces it to 1 on a box-less machine)
//   Enable Polar Cooler-> save_variables enable_polar_cooler, read by M1004 (-> M106 P4 -> output_pin polar_cooler)
// None of them live in the G-code, which is why a file sliced here and one sliced in Qidi Studio behave
// the same at the printer; the dialog's job is to set this state before the upload.

static std::string extract_error(const std::string& body, const std::string& error, unsigned status)
{
    // Moonraker errors: {"error": {"code": 400, "message": "..."}}
    try {
        auto j = json::parse(body, nullptr, false);
        if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message"))
            return j["error"]["message"].get<std::string>();
    } catch (...) {}
    if (!error.empty())
        return error;
    return (boost::format("HTTP %1%") % status).str();
}

QidiPrinterLink::QidiPrinterLink(const DynamicPrintConfig& config)
    : m_host(config.opt_string("print_host"))
    , m_apikey(config.opt_string("printhost_apikey"))
{}

std::string QidiPrinterLink::make_url(const std::string& path) const
{
    std::string host = m_host;
    if (host.find("http://") != 0 && host.find("https://") != 0)
        host = "http://" + host;
    if (!host.empty() && host.back() == '/')
        host.pop_back();
    return host + "/" + path;
}

void QidiPrinterLink::set_auth(Http& http) const
{
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
}

bool QidiPrinterLink::get_json(const std::string& path, std::string& body, wxString& error) const
{
    bool ok  = true;
    auto url = make_url(path);
    auto http = Http::get(url);
    set_auth(http);
    http.timeout_connect(4)
        .timeout_max(12)
        .on_complete([&](std::string resp, unsigned) { body = std::move(resp); })
        .on_error([&](std::string resp, std::string err, unsigned status) {
            ok    = false;
            error = GUI::from_u8(extract_error(resp, err, status));
            BOOST_LOG_TRIVIAL(warning) << boost::format("QidiPrinterLink: GET %1% failed: %2% (HTTP %3%)") % url % err % status;
        })
        .perform_sync();
    return ok;
}

bool QidiPrinterLink::post_json(const std::string& path, const std::string& body, wxString& error) const
{
    bool ok  = true;
    auto url = make_url(path);
    auto http = Http::post(url);
    set_auth(http);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .timeout_connect(4)
        .timeout_max(20)
        .on_complete([&](std::string, unsigned) {})
        .on_error([&](std::string resp, std::string err, unsigned status) {
            ok    = false;
            error = GUI::from_u8(extract_error(resp, err, status));
            BOOST_LOG_TRIVIAL(warning) << boost::format("QidiPrinterLink: POST %1% failed: %2% (HTTP %3%) body `%4%`") % url % err % status % resp;
        })
        .perform_sync();
    return ok;
}

bool QidiPrinterLink::run_gcode(const std::string& script, wxString& error) const
{
    // POST /printer/gcode/script {"script": "..."} - Moonraker returns {"result": "ok"} once Klipper ran it.
    json body;
    body["script"] = script;
    BOOST_LOG_TRIVIAL(info) << "QidiPrinterLink: " << script;
    return post_json("printer/gcode/script", body.dump(), error);
}

bool QidiPrinterLink::query(QidiPrinterState& state, wxString& error) const
{
    state = QidiPrinterState{};
    std::string body;

    // 1. Is anyone there? /printer/info also carries the Klipper state and the host name.
    if (!get_json("printer/info", body, error))
        return false;
    try {
        auto j = json::parse(body);
        if (j.contains("result")) {
            const auto& r = j["result"];
            state.klippy_state = r.value("state", "");
            state.hostname     = r.value("hostname", "");
        }
    } catch (const std::exception& e) {
        error = GUI::format_wxstr(_L("Unexpected reply from the printer: %s"), e.what());
        return false;
    }
    state.online = true;

    // 2. Print state, the saved variables the start macros read, the filament sensor, the mesh switch.
    //    Klipper rejects a query naming an object it doesn't have, so unknown objects are simply absent
    //    from the reply rather than an error.
    const std::string objects = "printer/objects/query?print_stats&save_variables"
                                "&filament_switch_sensor%20filament_switch_sensor&gcode_macro%20_km_globals";
    if (get_json(objects, body, error)) {
        try {
            auto        j      = json::parse(body);
            const auto& status = j.at("result").at("status");
            if (status.contains("print_stats"))
                state.print_state = status["print_stats"].value("state", "");
            if (status.contains("save_variables") && status["save_variables"].contains("variables")) {
                const auto& v = status["save_variables"]["variables"];
                if (v.contains("box_count") && v["box_count"].is_number())
                    state.box_count = std::max(0, v["box_count"].get<int>());
                if (v.contains("enable_box")) {
                    state.has_box_switch = true;
                    state.enable_box     = v["enable_box"].is_number() ? v["enable_box"].get<int>() != 0 : v["enable_box"].is_boolean() && v["enable_box"].get<bool>();
                }
                if (v.contains("enable_polar_cooler")) {
                    state.has_polar_cooler = true;
                    const auto& p          = v["enable_polar_cooler"];
                    state.polar_cooler     = p.is_number() ? p.get<int>() != 0 : (p.is_boolean() ? p.get<bool>() : true);
                }
                if (v.contains("filament_slot16")) {
                    const auto& f = v["filament_slot16"];
                    state.external_filament = f.is_string() ? f.get<std::string>() : (f.is_number() ? std::to_string(f.get<int>()) : "");
                }
            }
            if (status.contains("filament_switch_sensor filament_switch_sensor"))
                state.filament_detected = status["filament_switch_sensor filament_switch_sensor"].value("filament_detected", false);
            if (status.contains("gcode_macro _km_globals")) {
                const auto& km = status["gcode_macro _km_globals"];
                if (km.contains("bedmesh_before_print")) {
                    state.has_bed_leveling = true;
                    const auto& b          = km["bedmesh_before_print"];
                    state.bed_leveling     = b.is_boolean() ? b.get<bool>() : (b.is_number() ? b.get<int>() != 0 : true);
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "QidiPrinterLink: could not parse objects/query: " << e.what();
        }
    }

    // 3. Timelapse component (optional on a Klipper host; standard on Qidi's image).
    wxString tl_error;
    if (get_json("machine/timelapse/settings", body, tl_error)) {
        try {
            auto j = json::parse(body);
            if (j.contains("result") && j["result"].contains("enabled")) {
                state.has_timelapse = true;
                state.timelapse     = j["result"]["enabled"].get<bool>();
            }
        } catch (...) {}
    }
    error.clear();
    return true;
}

bool QidiPrinterLink::apply(const QidiSendOptions& o, wxString& error) const
{
    wxString step_error;
    if (o.write_polar_cooler) {
        if (!run_gcode(std::string("SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=") + (o.polar_cooler ? "1" : "0"), step_error)) {
            error = GUI::format_wxstr(_L("Could not set the Polar Cooler option on the printer: %s"), step_error);
            return false;
        }
    }
    if (o.write_enable_box) {
        if (!run_gcode(std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (o.enable_box ? "1" : "0"), step_error)) {
            error = GUI::format_wxstr(_L("Could not set the Box option on the printer: %s"), step_error);
            return false;
        }
    }
    if (o.write_bed_leveling) {
        // The variable is declared as a Python bool in the macro, so keep its type.
        if (!run_gcode(std::string("SET_GCODE_VARIABLE MACRO=_km_globals VARIABLE=bedmesh_before_print VALUE=") + (o.bed_leveling ? "True" : "False"), step_error)) {
            error = GUI::format_wxstr(_L("Could not set Auto Bed Leveling on the printer: %s"), step_error);
            return false;
        }
    }
    if (o.write_timelapse) {
        json body;
        body["enabled"] = o.timelapse;
        if (!post_json("machine/timelapse/settings", body.dump(), step_error)) {
            error = GUI::format_wxstr(_L("Could not set Timelapse on the printer: %s"), step_error);
            return false;
        }
    }
    return true;
}

} // namespace Slic3r
