#pragma once

// Orca/ATN: webview panel hosting the Ask The Nozzle assistant.
// The page (local fallback or https://askthenozzle.com/slicer) talks to the
// slicer through the "wx" script message handler; see on_script_message for
// the supported commands.

#include <wx/panel.h>
#include <string>

class wxWebView;
class wxWebViewEvent;

namespace Slic3r {
namespace GUI {

class AtnPanel : public wxPanel
{
public:
    AtnPanel(wxWindow* parent);

    void load_url(const wxString& url);

    // Orca/ATN: tell the embedded page which workflow mode to show
    // ("prepare" = pre-slice questions, "preview" = post-slice report + chat).
    void set_mode(const std::string& mode);
    // Fired when a slice finishes successfully: the page auto-runs pre-flight.
    void on_slice_complete();
    // Fired when a job is dispatched to the farm: the page records it in the learning diary.
    void on_sent_to_farm();

private:
    void on_script_message(wxWebViewEvent& evt);
    void send_to_page(const std::string& json_payload);

    std::string build_context_json() const;
    void        handle_highlight_setting(const std::string& key);
    void        handle_highlight_tool(const std::string& tool);
    void        handle_set_setting(const std::string& key, const std::string& value);
    void        handle_request_preflight();
    void        handle_capture_model();
    void        handle_apply_optimized(const std::string& b64, size_t raw_size);

    wxWebView* m_browser{ nullptr };
};

} // namespace GUI
} // namespace Slic3r
