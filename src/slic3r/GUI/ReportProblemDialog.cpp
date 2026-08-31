#include "ReportProblemDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"   // data_dir()
#include "libslic3r_version.h"   // SoftFever_VERSION (generated into the build tree)

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/utils.h>                     // wxGetOsDescription

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <vector>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

// The cloud endpoint that emails the report. Uses the SAME origin the assistant panel
// uses (ATN_PANEL_URL env / app config / hosted), reduced to scheme://host + the path.
static std::string atn_report_url()
{
    std::string base;
    if (const char* e = std::getenv("ATN_PANEL_URL"); e != nullptr && *e != 0) base = e;
    if (base.empty() && wxGetApp().app_config) base = wxGetApp().app_config->get("atn_panel_url");
    if (base.empty()) base = "https://askthenozzle.com";
    if (auto sch = base.find("://"); sch != std::string::npos) {
        if (auto slash = base.find('/', sch + 3); slash != std::string::npos)
            base = base.substr(0, slash);
    }
    return base + "/api/slicer/report-problem";
}

// Bundle the most useful diagnostics from data_dir()/log into one temp file: the newest crash
// dump (the fault stack) plus the two newest debug logs. A crash CLOSES the app, so by the time
// the user files a report the "newest" debug log is often the post-restart session and the crash
// itself is in the PREVIOUS one — so we include both. The crash dump is written LAST so it
// survives the server's tail-trim. Returns the temp path, or empty if there's nothing to send.
static fs::path build_diag_bundle()
{
    boost::system::error_code ec;
    const fs::path dir = fs::path(data_dir()) / "log";
    if (!fs::is_directory(dir, ec)) return {};

    std::vector<std::pair<std::time_t, fs::path>> crashes, debugs;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!fs::is_regular_file(it->status())) continue;
        const std::string name = it->path().filename().string();
        boost::system::error_code te;
        std::time_t t = fs::last_write_time(it->path(), te);
        if (te) continue;
        if      (name.rfind("crash_", 0) == 0) crashes.emplace_back(t, it->path());
        else if (name.rfind("debug_", 0) == 0) debugs.emplace_back(t, it->path());
    }
    auto newest_first = [](std::vector<std::pair<std::time_t, fs::path>>& v) {
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    };
    newest_first(crashes);
    newest_first(debugs);

    std::string out;
    auto append = [&](const fs::path& p, const char* label) {
        boost::system::error_code e2;
        if (p.empty() || !fs::exists(p, e2)) return;
        out += std::string("\n===== ") + label + ": " + p.filename().string() + " =====\n";
        try { fs::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); out += ss.str(); }
        catch (...) {}
    };
    // Debug logs first (context), newest crash LAST (so the tail-trim keeps the fault stack).
    for (size_t i = 0; i < debugs.size() && i < 2; ++i)
        append(debugs[i].second, i == 0 ? "DEBUG (newest)" : "DEBUG (previous)");
    if (!crashes.empty())
        append(crashes.front().second, "CRASH");
    if (out.empty()) return {};

    const fs::path tmp = fs::temp_directory_path() / "atn_report_diag.log";
    try { fs::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << out; }
    catch (...) { return {}; }
    return tmp;
}

ReportProblemDialog::ReportProblemDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Report a Problem"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    const int em  = wxGetApp().em_unit();
    const int pad = FromDIP(12);
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(this, wxID_ANY, _L("Report a problem"));
    { auto f = title->GetFont(); f.SetPointSize(f.GetPointSize() + 4); f.MakeBold(); title->SetFont(f); }
    root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    auto* sub = new wxStaticText(this, wxID_ANY,
        _L("Tell us what went wrong and we'll investigate. Your slicer version and system "
           "details are included automatically."));
    sub->Wrap(FromDIP(420));
    root->Add(sub, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    root->Add(new wxStaticText(this, wxID_ANY, _L("What happened?")), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    m_message = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(40 * em, FromDIP(120)), wxTE_MULTILINE);
    m_message->SetHint(_L("What you did, what you expected, and what happened instead…"));
    root->Add(m_message, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

    root->Add(new wxStaticText(this, wxID_ANY, _L("Your email (so we can reply)")), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    m_email = new wxTextCtrl(this, wxID_ANY, "");
    m_email->SetHint(_L("you@example.com"));
    // Prefill from the signed-in farm account, if any.
    if (wxGetApp().app_config) {
        const std::string acct = wxGetApp().app_config->get("farm", "account");
        if (!acct.empty()) m_email->SetValue(wxString::FromUTF8(acct));
    }
    root->Add(m_email, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    m_include_log = new wxCheckBox(this, wxID_ANY, _L("Include a recent diagnostic log (recommended)"));
    m_include_log->SetValue(true);
    root->Add(m_include_log, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    m_status = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new Button(this, _L("Cancel"), wxEmptyString, 0, 0, wxID_CANCEL);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (IsModal()) EndModal(wxID_CANCEL); else Close(); });
    m_send = new Button(this, _L("Send report"));
    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { do_send(); });
    btns->Add(cancel, 0, wxRIGHT, pad / 2);
    btns->Add(m_send, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, pad);

    SetSizerAndFit(root);
    CenterOnParent();
    m_message->SetFocus();
}

ReportProblemDialog::~ReportProblemDialog()
{
    if (m_alive) *m_alive = false;
}

void ReportProblemDialog::do_send()
{
  try {
    const std::string msg = m_message->GetValue().Trim().Trim(false).ToStdString();
    if (msg.empty()) {
        m_status->SetForegroundColour(wxColour(180, 60, 50));
        m_status->SetLabel(_L("Please describe the problem first.")); Layout(); return;
    }
    m_send->Disable();
    m_status->SetForegroundColour(wxColour(120, 120, 120));
    m_status->SetLabel(_L("Sending…")); Layout();

    auto http = Http::post(atn_report_url());
    http.form_add("message", msg)
        .form_add("email", m_email->GetValue().Trim().Trim(false).ToStdString())
        .form_add("version", std::string(SoftFever_VERSION))
        .form_add("os_info", wxGetOsDescription().ToStdString())
        .form_add("source", "help-menu");
    if (m_include_log && m_include_log->IsChecked()) {
        const fs::path bundle = build_diag_bundle();
        boost::system::error_code ec;
        if (!bundle.empty() && fs::exists(bundle, ec))
            http.form_add_file("log_file", bundle, "slicer-logs.txt");
    }
    http.timeout_connect(5).timeout_max(20)
        .on_complete([this, alive = m_alive](std::string, unsigned status) {
            wxGetApp().CallAfter([this, alive, status]() {
                if (!*alive) return;
                if (status >= 200 && status < 300) {
                    m_status->SetForegroundColour(wxColour(30, 150, 80));
                    m_status->SetLabel(wxString::FromUTF8("\xE2\x9C\x93 ")
                                       + _L("Thanks — your report has been sent. You can close this window."));
                } else {
                    m_send->Enable();
                    m_status->SetForegroundColour(wxColour(180, 60, 50));
                    m_status->SetLabel(wxString::Format(_L("Couldn't send (%u). Please try again."), status));
                }
                Layout();
            });
        })
        .on_error([this, alive = m_alive](std::string, std::string, unsigned) {
            wxGetApp().CallAfter([this, alive]() {
                if (!*alive) return;
                m_send->Enable();
                m_status->SetForegroundColour(wxColour(180, 60, 50));
                m_status->SetLabel(_L("Couldn't reach the server — check your connection and try again."));
                Layout();
            });
        })
        .perform();
  } catch (...) {
      if (m_send) m_send->Enable();
      if (m_status) {
          m_status->SetForegroundColour(wxColour(180, 60, 50));
          m_status->SetLabel(_L("Send failed. Please try again."));
          Layout();
      }
  }
}

} // namespace GUI
} // namespace Slic3r
