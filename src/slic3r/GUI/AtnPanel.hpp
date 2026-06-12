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

private:
    void on_script_message(wxWebViewEvent& evt);
    void send_to_page(const std::string& json_payload);

    std::string build_context_json() const;
    void        handle_highlight_setting(const std::string& key);
    void        handle_set_setting(const std::string& key, const std::string& value);

    wxWebView* m_browser{ nullptr };
};

} // namespace GUI
} // namespace Slic3r
