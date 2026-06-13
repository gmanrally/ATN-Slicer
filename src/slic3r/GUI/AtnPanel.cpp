#include "AtnPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <wx/base64.h>
#include <wx/sizer.h>
#include <wx/webview.h>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

AtnPanel::AtnPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
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
}

void AtnPanel::load_url(const wxString& url)
{
    if (m_browser != nullptr)
        m_browser->LoadURL(url);
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
        } else if (command == "atn_set_setting") {
            handle_set_setting(root["data"]["key"].get<std::string>(), root["data"]["value"].get<std::string>());
        } else if (command == "atn_request_preflight") {
            handle_request_preflight();
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

    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr || !plate->is_slice_result_valid() || plate->get_slice_result() == nullptr) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "The current plate isn't sliced yet - slice it first, then run the report.";
        send_to_page(reply.dump());
        return;
    }

    const std::string path = plate->get_slice_result()->filename;
    if (path.empty() || !boost::filesystem::exists(path)) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "Sliced G-code file not found on disk.";
        send_to_page(reply.dump());
        return;
    }

    // Cap the size: the server's geometry walk also tops out around 50 MB, and a
    // very large base64 string is heavy to push across the webview bridge.
    const std::uintmax_t size = boost::filesystem::file_size(path);
    if (size > 40ull * 1024 * 1024) {
        reply["data"]["ok"]      = false;
        reply["data"]["message"] = "This G-code is too large for the in-app pre-flight; use the askthenozzle.com tool instead.";
        send_to_page(reply.dump());
        return;
    }

    boost::nowide::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();

    reply["data"]["ok"]   = true;
    reply["data"]["name"] = boost::filesystem::path(path).filename().string();
    reply["data"]["b64"]  = wxBase64Encode(bytes.data(), bytes.size()).ToStdString();
    send_to_page(reply.dump());
}

} // namespace GUI
} // namespace Slic3r
