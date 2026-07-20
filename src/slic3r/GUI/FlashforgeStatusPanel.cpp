#include "FlashforgeStatusPanel.hpp"

#include <thread>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/gauge.h>
#include <wx/settings.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "FlashforgeCameraView.hpp"
#include "../Utils/Flashforge.hpp"

namespace Slic3r {
namespace GUI {

static constexpr int POLL_INTERVAL_MS = 3000;

static wxString fmt_temp(double cur, double target)
{
    if (cur < 0)
        return _L("--");
    if (target > 0)
        return wxString::Format("%.0f / %.0f °C", cur, target);
    return wxString::Format("%.0f °C", cur);
}

static wxString fmt_duration(double seconds)
{
    if (seconds < 0)
        return {};
    const long s = static_cast<long>(seconds);
    const long h = s / 3600, m = (s % 3600) / 60;
    if (h > 0)
        return wxString::Format("%ldh %ldm", h, m);
    return wxString::Format("%ldm", m);
}

// The printer's raw status string -> a human label. Falls back to the raw value so an unknown state
// is shown verbatim rather than hidden.
static wxString pretty_state(const std::string& raw)
{
    std::string s = raw;
    for (auto& c : s) c = (char) tolower((unsigned char) c);
    if (s == "ready" || s == "idle")   return _L("Ready");
    if (s == "printing" || s == "busy") return _L("Printing");
    if (s == "pause" || s == "paused") return _L("Paused");
    if (s == "completed")              return _L("Completed");
    if (s == "cancel" || s == "cancelled") return _L("Cancelled");
    if (s == "error")                  return _L("Error");
    if (raw.empty())                   return _L("Unknown");
    return from_u8(raw);
}

FlashforgeStatusPanel::FlashforgeStatusPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
    , m_timer(this)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    build_ui();
    Bind(wxEVT_TIMER, &FlashforgeStatusPanel::on_timer, this);
}

FlashforgeStatusPanel::~FlashforgeStatusPanel()
{
    // Disarm any in-flight worker: its completion callback checks this before touching the panel.
    *m_alive = false;
    m_timer.Stop();
    if (m_camera)
        m_camera->Stop();                // stop the stream thread promptly rather than at child teardown
}

void FlashforgeStatusPanel::build_ui()
{
    const int gap = FromDIP(6);
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto title_font = GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 3);
    title_font.MakeBold();

    // Live camera view -- hidden until the printer reports a stream URL, then it is the hero of the
    // tab. Proportion 1 so it grows with the window; the status strip below stays at natural height.
    m_camera = new FlashforgeCameraView(this);
    m_camera->SetMinSize(FromDIP(wxSize(-1, 260)));
    m_camera->Show(false);
    root->Add(m_camera, 1, wxEXPAND | wxALL, FromDIP(16));

    m_state = new wxStaticText(this, wxID_ANY, _L("Connecting…"));
    m_state->SetFont(title_font);
    root->Add(m_state, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_conn = new wxStaticText(this, wxID_ANY, _L("Reading status from the printer over your local network…"));
    root->Add(m_conn, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_gauge = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, wxSize(-1, FromDIP(10)));
    root->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_progress = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_progress, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_file = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_file, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto* temps = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle  = new wxStaticText(this, wxID_ANY, _L("Nozzle: --"));
    m_bed     = new wxStaticText(this, wxID_ANY, _L("Bed: --"));
    m_chamber = new wxStaticText(this, wxID_ANY, wxEmptyString);
    temps->Add(m_nozzle, 0, wxRIGHT, FromDIP(24));
    temps->Add(m_bed, 0, wxRIGHT, FromDIP(24));
    temps->Add(m_chamber, 0);
    root->Add(temps, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    root->AddStretchSpacer(1);

    m_footer = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_footer->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    root->Add(m_footer, 0, wxALL, FromDIP(16));

    (void) gap;
    SetSizer(root);
}

void FlashforgeStatusPanel::set_printer(const DynamicPrintConfig& config)
{
    m_config      = config;
    m_have_printer = true;
    if (IsShownOnScreen()) {
        poll_now();                     // immediate reading, don't wait a full interval
        m_timer.Start(POLL_INTERVAL_MS);
    }
}

bool FlashforgeStatusPanel::Show(bool show)
{
    const bool ret = wxPanel::Show(show);
    if (show && m_have_printer) {
        poll_now();
        m_timer.Start(POLL_INTERVAL_MS);
    } else {
        m_timer.Stop();
        if (m_camera)
            m_camera->Stop();            // don't pull frames while the tab is hidden
    }
    return ret;
}

void FlashforgeStatusPanel::on_timer(wxTimerEvent&) { poll_now(); }

void FlashforgeStatusPanel::poll_now()
{
    if (!m_have_printer)
        return;

    // The status/camera API (port 8898) needs the printer's ACCESS CODE (its LAN pairing/check code),
    // stored in the "API Key / Password" field, on top of the serial number. The serial-only
    // connection Test (port 8899) works WITHOUT it, so an empty access code is a common first-run
    // state. Tell the user exactly what to fill in rather than firing a request that can only fail.
    const std::string code   = m_config.has("printhost_apikey") ? m_config.opt_string("printhost_apikey") : "";
    const std::string serial = m_config.has("flashforge_serial_number") ? m_config.opt_string("flashforge_serial_number") : "";
    if (code.empty() || serial.empty()) {
        m_state->SetLabel(_L("Access code needed"));
        m_conn->SetLabel(_L("Enter the printer's access code (shown on the printer under LAN mode) in the "
                            "API Key / Password field of the printer settings, then reopen the Device tab."));
        m_gauge->SetValue(0);
        m_progress->SetLabel(wxEmptyString);
        m_file->SetLabel(wxEmptyString);
        m_nozzle->SetLabel(_L("Nozzle: --"));
        m_bed->SetLabel(_L("Bed: --"));
        m_chamber->SetLabel(wxEmptyString);
        if (m_camera) { m_camera->Stop(); m_camera->Show(false); }
        Layout();
        return;
    }

    bool expected = false;
    if (!m_in_flight.compare_exchange_strong(expected, true))
        return;                          // a request is already running; skip this tick

    // Copy everything the worker needs by value -- it must not touch GUI state or the live config.
    DynamicPrintConfig cfg = m_config;
    auto alive = m_alive;
    auto* self = this;

    std::thread([cfg = std::move(cfg), alive, self]() mutable {
        FlashforgeStatus st;
        wxString msg;
        {
            Flashforge ff(&cfg);
            ff.fetch_status(st, msg);    // blocking HTTP; st.valid is the success flag
        }
        // Marshal back to the GUI thread. If the panel is gone by then, *alive is false and we skip.
        wxGetApp().CallAfter([self, alive, st, msg]() {
            if (!*alive)
                return;
            self->m_in_flight = false;

            if (!st.valid) {
                self->m_state->SetLabel(_L("Offline"));
                self->m_conn->SetLabel(msg.empty()
                    ? _L("Could not reach the printer on your local network. Check that it is powered on and on the same network.")
                    : msg);
                self->m_gauge->SetValue(0);
                self->m_progress->SetLabel(wxEmptyString);
                self->m_file->SetLabel(wxEmptyString);
                self->m_nozzle->SetLabel(_L("Nozzle: --"));
                self->m_bed->SetLabel(_L("Bed: --"));
                self->m_chamber->SetLabel(wxEmptyString);
                if (self->m_camera) { self->m_camera->Stop(); self->m_camera->Show(false); }
                self->Layout();
                return;
            }

            self->m_state->SetLabel(pretty_state(st.state));
            self->m_conn->SetLabel(_L("Connected over local network"));

            // Show the live view when the printer reports a stream URL; Start() is a no-op if that URL
            // is already streaming, so this can run every poll without tearing the feed down.
            if (self->m_camera) {
                if (!st.camera_url.empty()) {
                    self->m_camera->Start(from_u8(st.camera_url));
                    if (!self->m_camera->IsShown()) { self->m_camera->Show(true); self->Layout(); }
                } else if (self->m_camera->IsShown()) {
                    self->m_camera->Stop();
                    self->m_camera->Show(false);
                    self->Layout();
                }
            }

            if (st.progress >= 0.0) {
                self->m_gauge->SetValue(std::min(1000, std::max(0, int(st.progress * 1000))));
                wxString line = wxString::Format("%d%%", int(st.progress * 100.0 + 0.5));
                if (st.layer >= 0 && st.total_layers > 0)
                    line += wxString::Format("   |   %s %d/%d", _L("layer"), st.layer, st.total_layers);
                const wxString eta = fmt_duration(st.est_time_s);
                if (!eta.empty())
                    line += "   |   ~" + eta + " " + _L("left");
                self->m_progress->SetLabel(line);
            } else {
                self->m_gauge->SetValue(0);
                self->m_progress->SetLabel(wxEmptyString);
            }

            self->m_file->SetLabel(st.file_name.empty() ? wxString(wxEmptyString)
                                                        : _L("File: ") + from_u8(st.file_name));
            self->m_nozzle->SetLabel(_L("Nozzle: ") + fmt_temp(st.nozzle_temp, st.nozzle_target));
            self->m_bed->SetLabel(_L("Bed: ") + fmt_temp(st.bed_temp, st.bed_target));
            self->m_chamber->SetLabel(st.chamber_temp > 0
                ? _L("Chamber: ") + wxString::Format("%.0f °C", st.chamber_temp)
                : wxString(wxEmptyString));

            wxString footer;
            if (!st.firmware.empty())
                footer = _L("Firmware ") + from_u8(st.firmware);
            if (!st.error_code.empty() && st.error_code != "0") {
                if (!footer.empty()) footer += "   |   ";
                footer += _L("Error ") + from_u8(st.error_code);
            }
            self->m_footer->SetLabel(footer);
            self->Layout();
        });
    }).detach();
}

} // namespace GUI
} // namespace Slic3r
