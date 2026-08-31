#include "FlashforgeStatusPanel.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>
#include <utility>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/gauge.h>
#include <wx/settings.h>
#include <wx/button.h>
#include <wx/spinctrl.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>

#include <boost/log/trivial.hpp>

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

static std::string to_lower(std::string s) { for (auto& c : s) c = (char) tolower((unsigned char) c); return s; }

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

void FlashforgeStatusPanel::on_expand_camera()
{
    if (m_camera_url.empty())
        return;

    // FlashForge cameras (AD5X / 5M) serve a SINGLE client: a second connection makes the printer
    // drop one feed, so the pop-out would just show "stream ended". Worse, the status poll keeps
    // calling m_camera->Start() on the docked feed; once its stream is stolen that stops being a
    // no-op and churns socket close + a fresh worker thread every tick inside this modal loop —
    // which is what was crashing the app on a second open. So hand the single stream to the pop-out:
    // pause the docked feed (and tell the poll to leave it alone) while the dialog is open, then
    // resume it on close. Resizable + maximisable so the user can make it as big as they like.
    const wxString url = m_camera_url;   // capture: the poll won't update m_camera_url while expanded
    m_camera_expanded = true;
    if (m_camera) m_camera->Stop();      // free the single connection for the pop-out

    {
        wxDialog dlg(this, wxID_ANY, _L("Camera"), wxDefaultPosition, FromDIP(wxSize(960, 720)),
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX);
        dlg.SetBackgroundColour(*wxBLACK);

        auto* s    = new wxBoxSizer(wxVERTICAL);
        auto* view = new FlashforgeCameraView(&dlg);
        s->Add(view, 1, wxEXPAND);
        dlg.SetSizer(s);

        view->Start(url);
        dlg.CentreOnParent();
        dlg.ShowModal();
        view->Stop();                    // stop the stream before the dialog (and view) are destroyed
    }

    m_camera_expanded = false;
    if (m_camera && !m_camera_url.empty())
        m_camera->Start(m_camera_url);   // resume the docked feed on the now-free connection
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

    // ---- job controls (pause/resume + cancel), shown only while a job is active ----
    {
        auto* ctl = new wxBoxSizer(wxHORIZONTAL);
        m_btn_pauseresume = new wxButton(this, wxID_ANY, _L("Pause"));
        m_btn_cancel      = new wxButton(this, wxID_ANY, _L("Cancel"));
        m_btn_pauseresume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const bool paused = [&]{ const std::string s = to_lower(m_state_raw); return s == "pause" || s == "paused"; }();
            const std::string action = paused ? "continue" : "pause";
            BOOST_LOG_TRIVIAL(info) << "[Flashforge] pause/resume clicked; state_raw='" << m_state_raw
                                    << "' action=" << action;
            run_control([action](const Flashforge& ff, wxString& msg) { return ff.job_control(action, msg); },
                        paused ? _L("Resuming…") : _L("Pausing…"));
        });
        m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            BOOST_LOG_TRIVIAL(info) << "[Flashforge] cancel clicked; state_raw='" << m_state_raw << "'";
            run_control([](const Flashforge& ff, wxString& msg) { return ff.job_control("cancel", msg); },
                        _L("Cancelling…"));
        });
        m_btn_pauseresume->Show(false);
        m_btn_cancel->Show(false);
        ctl->Add(m_btn_pauseresume, 0, wxRIGHT, FromDIP(8));
        ctl->Add(m_btn_cancel, 0);
        root->Add(ctl, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- camera hero (shown only while the printer streams) ----
    // An Expand button pops the feed out into a large, resizable window: the docked panel shares
    // vertical space with every control card, so the inline view is necessarily small.
    m_btn_expand = new wxButton(this, wxID_ANY, _L("Expand"));
    m_btn_expand->SetToolTip(_L("Open the camera in a large, resizable window"));
    m_btn_expand->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_expand_camera(); });
    m_btn_expand->Show(false);   // a window item respects IsShown(), so this collapses cleanly when hidden
    root->Add(m_btn_expand, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxTOP, pad);

    m_camera = new FlashforgeCameraView(this);
    m_camera->SetMinSize(FromDIP(wxSize(-1, 380)));
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

        m_extra = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_extra->SetForegroundColour(pal.muted);
        c->Add(m_extra, 0, wxTOP, FromDIP(2));

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- temperature card (with editable nozzle/bed targets) ----
    {
        auto card_pair = make_card(_L("TEMPERATURE"));
        wxBoxSizer* c = card_pair.second;
        wxPanel*    card = card_pair.first;

        // spin/setbtn are null for a read-only row (e.g. Chamber, which the AD5M/AD5X can't heat).
        auto add_row = [this, card, c, pal](const wxString& label, wxStaticText*& cur, wxStaticText*& tgt,
                                            wxSpinCtrl** spin, wxButton** setbtn, int maxtemp) {
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
            if (spin && setbtn) {
                row->AddStretchSpacer(1);
                auto* s = new wxSpinCtrl(card, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                         FromDIP(wxSize(64, -1)), wxSP_ARROW_KEYS, 0, maxtemp, 0);
                auto* b = new wxButton(card, wxID_ANY, _L("Set"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                row->Add(s, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(4));
                row->Add(b, 0, wxALIGN_CENTRE_VERTICAL);
                *spin = s; *setbtn = b;
                m_controls.push_back(s);
                m_controls.push_back(b);
            }
            c->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(3));
        };
        add_row(_L("Nozzle"),  m_nozzle_cur,  m_nozzle_tgt,  &m_set_nozzle, &m_btn_set_nozzle, 360);
        add_row(_L("Bed"),     m_bed_cur,     m_bed_tgt,     &m_set_bed,    &m_btn_set_bed,    120);
        add_row(_L("Chamber"), m_chamber_cur, m_chamber_tgt, nullptr,       nullptr,           0);

        // Each Set sends only its own channel (TEMP_LEAVE keeps the others as-is); 0 disables the heater.
        m_btn_set_nozzle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const double t = m_set_nozzle->GetValue();
            run_control([t](const Flashforge& ff, wxString& msg) {
                return ff.set_temperatures(t, Flashforge::TEMP_LEAVE, Flashforge::TEMP_LEAVE, msg); },
                _L("Setting nozzle…"));
        });
        m_btn_set_bed->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const double t = m_set_bed->GetValue();
            run_control([t](const Flashforge& ff, wxString& msg) {
                return ff.set_temperatures(Flashforge::TEMP_LEAVE, t, Flashforge::TEMP_LEAVE, msg); },
                _L("Setting bed…"));
        });

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- print-tuning card: live feed-rate override + Z compensation ----
    {
        auto card_pair = make_card(_L("PRINT TUNING"));
        wxBoxSizer* c = card_pair.second;
        wxPanel*    card = card_pair.first;

        auto stepper = [this, card, c, pal](const wxString& label, wxStaticText*& val,
                                            std::function<void(int)> bump) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* l = new wxStaticText(card, wxID_ANY, label);
            l->SetForegroundColour(pal.muted);
            l->SetMinSize(FromDIP(wxSize(78, -1)));
            val = new wxStaticText(card, wxID_ANY, "--");
            { auto f = val->GetFont(); f.MakeBold(); val->SetFont(f); }
            val->SetForegroundColour(pal.value);
            val->SetMinSize(FromDIP(wxSize(72, -1)));
            auto* minus = new wxButton(card, wxID_ANY, "-", wxDefaultPosition, FromDIP(wxSize(34, -1)));
            auto* plus  = new wxButton(card, wxID_ANY, "+", wxDefaultPosition, FromDIP(wxSize(34, -1)));
            minus->Bind(wxEVT_BUTTON, [bump](wxCommandEvent&) { bump(-1); });
            plus->Bind(wxEVT_BUTTON,  [bump](wxCommandEvent&) { bump(+1); });
            row->Add(l,     0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(8));
            row->Add(val,   0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(8));
            row->Add(minus, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(4));
            row->Add(plus,  0, wxALIGN_CENTRE_VERTICAL);
            c->Add(row, 0, wxTOP | wxBOTTOM, FromDIP(3));
            m_controls.push_back(minus);
            m_controls.push_back(plus);
        };

        stepper(_L("Speed"), m_speed_val, [this](int dir) {
            const int v = std::min(150, std::max(50, m_cur_speed + dir * 10));
            if (v == m_cur_speed) return;
            m_cur_speed = v;
            m_speed_val->SetLabel(wxString::Format("%d%%", v));
            run_control([v](const Flashforge& ff, wxString& msg) { return ff.set_speed(v, msg); },
                        _L("Adjusting speed…"));
        });
        stepper(_L("Z-offset"), m_z_val, [this](int dir) {
            double v = std::min(5.0, std::max(-5.0, m_cur_z + dir * 0.025));
            if (std::abs(v - m_cur_z) < 1e-6) return;
            m_cur_z = v;
            m_z_val->SetLabel(wxString::Format("%.3f mm", v));
            run_control([v](const Flashforge& ff, wxString& msg) { return ff.set_z_compensation(v, msg); },
                        _L("Adjusting Z…"));
        });

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- chamber card: light + air circulation (filtration) ----
    {
        auto card_pair = make_card(_L("CHAMBER"));
        wxBoxSizer* c = card_pair.second;
        wxPanel*    card = card_pair.first;

        m_btn_light = new wxButton(card, wxID_ANY, _L("Light: Off"));
        m_btn_light->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const bool turn_on = (m_light_state != 1);
            m_light_state = turn_on ? 1 : 0;
            m_btn_light->SetLabel(turn_on ? _L("Light: On") : _L("Light: Off"));
            run_control([turn_on](const Flashforge& ff, wxString& msg) { return ff.set_light(turn_on, msg); },
                        turn_on ? _L("Light on…") : _L("Light off…"));
        });
        c->Add(m_btn_light, 0, wxBOTTOM, FromDIP(8));
        m_controls.push_back(m_btn_light);

        auto* filt = new wxStaticText(card, wxID_ANY, _L("Filtration (5M Pro)"));
        filt->SetForegroundColour(pal.muted);
        c->Add(filt, 0, wxBOTTOM, FromDIP(4));

        m_int_circ = new wxCheckBox(card, wxID_ANY, _L("Internal circulate"));
        m_ext_circ = new wxCheckBox(card, wxID_ANY, _L("External circulate"));
        m_int_circ->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            const bool on = e.IsChecked();
            run_control([on](const Flashforge& ff, wxString& msg) { return ff.set_circulation(on ? 1 : 0, -1, msg); },
                        _L("Filtration…"));
        });
        m_ext_circ->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            const bool on = e.IsChecked();
            run_control([on](const Flashforge& ff, wxString& msg) { return ff.set_circulation(-1, on ? 1 : 0, msg); },
                        _L("Filtration…"));
        });
        c->Add(m_int_circ, 0, wxBOTTOM, FromDIP(2));
        c->Add(m_ext_circ, 0);
        m_controls.push_back(m_int_circ);
        m_controls.push_back(m_ext_circ);

        root->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // ---- footer (firmware + error) ----
    m_footer = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_footer->SetForegroundColour(pal.muted);
    root->Add(m_footer, 0, wxALL, pad);

    SetSizer(root);
    set_controls_online(false);      // stay greyed until the first successful reading
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
        set_controls_online(false);
        if (m_camera) { m_camera->Stop(); m_camera->Show(false); }
        if (m_btn_expand) m_btn_expand->Show(false);
        m_camera_url.clear();
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
                self->set_controls_online(false);
                if (self->m_camera) { self->m_camera->Stop(); self->m_camera->Show(false); }
                if (self->m_btn_expand) self->m_btn_expand->Show(false);
                self->m_camera_url.clear();
                self->Layout();
                return;
            }

            self->m_state->SetLabel(pretty_state(st.state));
            self->m_state->SetForegroundColour(state_colour(st.state, pal));
            self->m_conn->SetLabel(_L("Connected over local network"));

            // Show the live view when the printer reports a stream URL; Start() is a no-op if that URL
            // is already streaming, so this can run every poll without tearing the feed down. Skipped
            // while the pop-out is open — it owns the single camera connection until it closes.
            if (self->m_camera && !self->m_camera_expanded) {
                if (!st.camera_url.empty()) {
                    self->m_camera_url = from_u8(st.camera_url);
                    self->m_camera->Start(self->m_camera_url);
                    if (!self->m_camera->IsShown()) {
                        self->m_camera->Show(true);
                        if (self->m_btn_expand) self->m_btn_expand->Show(true);
                        self->Layout();
                    }
                } else if (self->m_camera->IsShown()) {
                    self->m_camera->Stop();
                    self->m_camera->Show(false);
                    if (self->m_btn_expand) self->m_btn_expand->Show(false);
                    self->m_camera_url.clear();
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
            self->update_controls(st);
            self->Layout();
        });
    }).detach();
}

// Sync the interactive controls from a fresh reading: re-enable them, flip Pause/Resume + Cancel
// visibility with the print state, and adopt any live values the printer reported (falling back to
// the last user-set value when a field is absent, so the +/- steppers stay coherent between polls).
void FlashforgeStatusPanel::update_controls(const FlashforgeStatus& st)
{
    set_controls_online(true);

    if (st.state != m_state_raw)   // log state transitions (m_state_raw still holds the previous)
        BOOST_LOG_TRIVIAL(info) << "[Flashforge] state='" << st.state << "'";
    m_state_raw = st.state;
    const std::string s = to_lower(st.state);
    const bool printing = (s == "printing" || s == "busy" || s == "heating" || s == "prepare");
    const bool paused   = (s == "pause" || s == "paused");

    m_btn_pauseresume->Show(printing || paused);
    m_btn_pauseresume->SetLabel(paused ? _L("Resume") : _L("Pause"));
    m_btn_cancel->Show(printing || paused);

    if (st.speed_pct >= 0) m_cur_speed = st.speed_pct;
    m_speed_val->SetLabel(wxString::Format("%d%%", m_cur_speed));
    if (st.z_comp > -999.0) m_cur_z = st.z_comp;
    m_z_val->SetLabel(wxString::Format("%.3f mm", m_cur_z));

    if (st.light_on >= 0) m_light_state = st.light_on;
    m_btn_light->SetLabel(m_light_state == 1 ? _L("Light: On") : _L("Light: Off"));

    if (st.internal_fan >= 0) m_int_circ->SetValue(st.internal_fan == 1);
    if (st.external_fan >= 0) m_ext_circ->SetValue(st.external_fan == 1);

    // Seed the target editors with the printer's current targets, but never fight the user mid-type.
    if (m_set_nozzle && wxWindow::FindFocus() != m_set_nozzle && st.nozzle_target > 0)
        m_set_nozzle->SetValue(int(st.nozzle_target + 0.5));
    if (m_set_bed && wxWindow::FindFocus() != m_set_bed && st.bed_target > 0)
        m_set_bed->SetValue(int(st.bed_target + 0.5));

    // Material / speed line on the job card.
    if (m_extra) {
        wxString line;
        if (!st.material.empty()) line = from_u8(st.material);
        if (m_cur_speed != 100) {
            if (!line.empty()) line += "   |   ";
            line += wxString::Format("%d%% %s", m_cur_speed, _L("speed"));
        }
        m_extra->SetLabel(line);
    }
}

void FlashforgeStatusPanel::set_controls_online(bool online)
{
    for (wxWindow* w : m_controls)
        if (w) w->Enable(online);
    if (!online) {
        if (m_btn_pauseresume) m_btn_pauseresume->Show(false);
        if (m_btn_cancel)      m_btn_cancel->Show(false);
    }
}

void FlashforgeStatusPanel::run_control(std::function<bool(const Flashforge&, wxString&)> op, const wxString& busy_hint)
{
    if (!m_have_printer) {
        BOOST_LOG_TRIVIAL(info) << "[Flashforge] control skipped: no printer set";
        return;
    }
    const std::string code   = m_config.has("printhost_apikey") ? m_config.opt_string("printhost_apikey") : "";
    const std::string serial = m_config.has("flashforge_serial_number") ? m_config.opt_string("flashforge_serial_number") : "";
    if (code.empty() || serial.empty()) {
        BOOST_LOG_TRIVIAL(info) << "[Flashforge] control skipped: code_empty=" << code.empty()
                                << " serial_empty=" << serial.empty();
        // Never no-op silently: tell the user why the control did nothing.
        m_conn->SetForegroundColour(wxColour(214, 74, 74));
        m_conn->SetLabel(_L("Enter the printer's access code (shown on the printer in LAN mode) in printer settings to control it."));
        Layout();
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[Flashforge] control dispatch: " << busy_hint.ToStdString();

    if (!busy_hint.empty()) {
        m_conn->SetForegroundColour(palette().muted);
        m_conn->SetLabel(busy_hint);
        Layout();
    }

    // Copy the config for the worker; it must not touch GUI state or the live config (same rule as poll_now).
    DynamicPrintConfig cfg = m_config;
    auto  alive = m_alive;
    auto* self  = this;
    std::thread([cfg = std::move(cfg), alive, self, op = std::move(op)]() mutable {
        wxString msg;
        bool     ok;
        {
            Flashforge ff(&cfg);
            ok = op(ff, msg);
        }
        wxGetApp().CallAfter([self, alive, ok, msg]() {
            if (!*alive)
                return;
            if (!ok) {
                self->m_conn->SetForegroundColour(wxColour(214, 74, 74));
                self->m_conn->SetLabel(msg.empty() ? _L("The printer rejected that command.") : msg);
                self->Layout();
            }
            self->poll_now();   // reflect the new state (or revert an optimistic guess) right away
        });
    }).detach();
}

} // namespace GUI
} // namespace Slic3r
