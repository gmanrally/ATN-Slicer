#include "FlashforgeStatusPanel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

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

// --- theme ------------------------------------------------------------------
namespace {
struct Palette { wxColour page, card, value, muted, accent; };

Palette palette()
{
    Palette p;
    if (wxGetApp().dark_mode()) {
        p.page   = wxColour(28, 28, 30);
        p.card   = wxColour(45, 45, 48);
        p.value  = wxColour(236, 236, 240);
        p.muted  = wxColour(150, 152, 160);
        p.accent = wxColour(56, 150, 255);
    } else {
        p.page   = wxColour(250, 250, 252);
        p.card   = wxColour(244, 244, 247);
        p.value  = wxColour(28, 28, 32);
        p.muted  = wxColour(120, 122, 130);
        p.accent = wxColour(23, 110, 214);
    }
    return p;
}

// State -> a status colour so "Printing", "Error", etc. read at a glance.
wxColour state_colour(const std::string& raw, const Palette& p)
{
    std::string s = raw;
    for (auto& c : s) c = (char) tolower((unsigned char) c);
    if (s == "printing" || s == "busy")                   return p.accent;
    if (s == "ready" || s == "idle" || s == "completed")  return wxColour(38, 170, 90);
    if (s == "pause" || s == "paused")                    return wxColour(220, 160, 40);
    if (s == "error")                                     return wxColour(214, 74, 74);
    return p.value;
}
} // namespace

static wxString fmt_cur_temp(double cur) { return cur < 0 ? wxString("--") : wxString::Format("%.0f", cur) + wxString::FromUTF8("°C"); }
static wxString fmt_tgt_temp(double tgt) { return tgt >= 0 ? "/ " + wxString::Format("%.0f", tgt) + wxString::FromUTF8("°C") : wxString(); }

static void set_temp(wxStaticText* cur, wxStaticText* tgt, double c, double t)
{
    cur->SetLabel(fmt_cur_temp(c));
    tgt->SetLabel(fmt_tgt_temp(t));
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
    SetBackgroundColour(palette().page);
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
    const Palette pal = palette();
    const int     pad = FromDIP(14);
    auto*         root = new wxBoxSizer(wxVERTICAL);

    // Card factory: a filled panel with a small-caps title; returns its inner content sizer to fill.
    auto make_card = [this, pal, pad](const wxString& title) -> std::pair<wxPanel*, wxBoxSizer*> {
        auto* card = new wxPanel(this, wxID_ANY);
        card->SetBackgroundColour(pal.card);
        auto* v = new wxBoxSizer(wxVERTICAL);
        if (!title.empty()) {
            auto* t = new wxStaticText(card, wxID_ANY, title);
            auto  f = t->GetFont(); f.MakeBold(); f.SetPointSize(std::max(6, f.GetPointSize() - 1));
            t->SetFont(f);
            t->SetForegroundColour(pal.muted);
            v->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        }
        auto* content = new wxBoxSizer(wxVERTICAL);
        v->Add(content, 1, wxEXPAND | wxALL, pad);
        card->SetSizer(v);
        return {card, content};
    };

    // ---- status header ----
    auto head_font = GetFont();
    head_font.SetPointSize(head_font.GetPointSize() + 4);
    head_font.MakeBold();
    m_state = new wxStaticText(this, wxID_ANY, _L("Connecting…"));
    m_state->SetFont(head_font);
    root->Add(m_state, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    m_conn = new wxStaticText(this, wxID_ANY, _L("Reading status from the printer over your local network…"));
    m_conn->SetForegroundColour(pal.muted);
    root->Add(m_conn, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(3));

    // ---- camera hero (shown only while the printer streams) ----
    m_camera = new FlashforgeCameraView(this);
    m_camera->SetMinSize(FromDIP(wxSize(-1, 300)));
    m_camera->Show(false);
    root->Add(m_camera, 1, wxEXPAND | wxALL, pad);

    // ---- print-job card ----
    {
        auto card_pair = make_card(_L("PRINT JOB"));
        wxBoxSizer* c = card_pair.second;
        wxPanel*    card = card_pair.first;

        m_file = new wxStaticText(card, wxID_ANY, _L("No active job"));
        { auto f = m_file->GetFont(); f.MakeBold(); f.SetPointSize(f.GetPointSize() + 1); m_file->SetFont(f); }
        m_file->SetForegroundColour(pal.value);
        c->Add(m_file, 0, wxBOTTOM, FromDIP(8));

        m_gauge = new wxGauge(card, wxID_ANY, 1000, wxDefaultPosition, wxSize(-1, FromDIP(8)));
        c->Add(m_gauge, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

        m_progress = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_progress->SetForegroundColour(pal.muted);
        c->Add(m_progress, 0);

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- temperature card ----
    {
        auto card_pair = make_card(_L("TEMPERATURE"));
        wxBoxSizer* c = card_pair.second;
        wxPanel*    card = card_pair.first;

        auto add_row = [this, card, c, pal](const wxString& label, wxStaticText*& cur, wxStaticText*& tgt) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* l   = new wxStaticText(card, wxID_ANY, label);
            l->SetForegroundColour(pal.muted);
            l->SetMinSize(FromDIP(wxSize(78, -1)));
            cur = new wxStaticText(card, wxID_ANY, "--");
            { auto f = cur->GetFont(); f.SetPointSize(f.GetPointSize() + 4); f.MakeBold(); cur->SetFont(f); }
            cur->SetForegroundColour(pal.accent);
            tgt = new wxStaticText(card, wxID_ANY, wxEmptyString);
            tgt->SetForegroundColour(pal.muted);
            row->Add(l,   0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(8));
            row->Add(cur, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(6));
            row->Add(tgt, 0, wxALIGN_CENTRE_VERTICAL);
            c->Add(row, 0, wxTOP | wxBOTTOM, FromDIP(3));
        };
        add_row(_L("Nozzle"),  m_nozzle_cur,  m_nozzle_tgt);
        add_row(_L("Bed"),     m_bed_cur,     m_bed_tgt);
        add_row(_L("Chamber"), m_chamber_cur, m_chamber_tgt);

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- footer (firmware + error) ----
    m_footer = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_footer->SetForegroundColour(pal.muted);
    root->Add(m_footer, 0, wxALL, pad);

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
        m_state->SetForegroundColour(palette().muted);
        m_conn->SetLabel(_L("Enter the printer's access code (shown on the printer under LAN mode) in the "
                            "API Key / Password field of the printer settings, then reopen the Device tab."));
        m_gauge->SetValue(0);
        m_progress->SetLabel(wxEmptyString);
        m_file->SetLabel(_L("No active job"));
        set_temp(m_nozzle_cur,  m_nozzle_tgt,  -1, -1);
        set_temp(m_bed_cur,     m_bed_tgt,     -1, -1);
        set_temp(m_chamber_cur, m_chamber_tgt, -1, -1);
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
            const Palette pal = palette();

            if (!st.valid) {
                self->m_state->SetLabel(_L("Offline"));
                self->m_state->SetForegroundColour(wxColour(214, 74, 74));
                self->m_conn->SetLabel(msg.empty()
                    ? _L("Could not reach the printer on your local network. Check that it is powered on and on the same network.")
                    : msg);
                self->m_gauge->SetValue(0);
                self->m_progress->SetLabel(wxEmptyString);
                self->m_file->SetLabel(_L("No active job"));
                set_temp(self->m_nozzle_cur,  self->m_nozzle_tgt,  -1, -1);
                set_temp(self->m_bed_cur,     self->m_bed_tgt,     -1, -1);
                set_temp(self->m_chamber_cur, self->m_chamber_tgt, -1, -1);
                if (self->m_camera) { self->m_camera->Stop(); self->m_camera->Show(false); }
                self->Layout();
                return;
            }

            self->m_state->SetLabel(pretty_state(st.state));
            self->m_state->SetForegroundColour(state_colour(st.state, pal));
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

            self->m_file->SetLabel(st.file_name.empty() ? _L("No active job")
                                                        : from_u8(st.file_name));
            set_temp(self->m_nozzle_cur,  self->m_nozzle_tgt,  st.nozzle_temp, st.nozzle_target);
            set_temp(self->m_bed_cur,     self->m_bed_tgt,     st.bed_temp,    st.bed_target);
            set_temp(self->m_chamber_cur, self->m_chamber_tgt, st.chamber_temp, -1);

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
