#include "SendToFarmDialog.hpp"
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/utils.h>   // wxLaunchDefaultBrowser

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

static const char* kDefaultFarmUrl = "https://farm.askthenozzle.com";

// Attach the cloud session token so the farm authorises the call. The local
// (127.0.0.1) farm ignores it; the cloud rejects calls without it (401).
static void add_token(Http& http, const std::string& token)
{
    if (!token.empty()) http.header("X-Farm-Token", token);
}

// A filename token looks like a part number if it ends in "-<digits>" with >=4
// digits (e.g. GMR-C-00069). Guards the filename fallback from treating an
// ordinary descriptive name as a part number.
static bool looks_like_part_number(const std::string& s)
{
    auto pos = s.rfind('-');
    if (pos == std::string::npos || pos + 1 >= s.size()) return false;
    const std::string tail = s.substr(pos + 1);
    if (tail.size() < 4) return false;
    for (char c : tail) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

SendToFarmDialog::SendToFarmDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Send to Farm"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    const int em = wxGetApp().em_unit();
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(0, 2, em / 2, em);
    grid->AddGrowableCol(1, 1);

    auto add_row = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    std::string init_url;
    if (auto* ac = wxGetApp().app_config) init_url = ac->get("farm", "url");
    if (init_url.empty()) init_url = kDefaultFarmUrl;
    m_farm_url = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(init_url), wxDefaultPosition, wxSize(24 * em, -1));
    m_farm_url->SetHint(wxString::FromUTF8(kDefaultFarmUrl));
    add_row(_L("Farm"), m_farm_url);

    m_part_number = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(24 * em, -1));
    m_part_number->SetHint(_L("e.g. GMR-C-00069 (blank = auto)"));
    add_row(_L("Part number"), m_part_number);

    m_name = new wxTextCtrl(this, wxID_ANY);
    m_name->SetHint(_L("Name for a new part (optional)"));
    add_row(_L("Name"), m_name);

    m_rev = new wxTextCtrl(this, wxID_ANY, "auto");
    add_row(_L("Revision"), m_rev);

    m_printer = new wxChoice(this, wxID_ANY);
    add_row(_L("Printer"), m_printer);

    root->Add(grid, 0, wxEXPAND | wxALL, em);

    // Which machine this plate was sliced for — filters the printer list below.
    m_compat = new wxStaticText(this, wxID_ANY, "");
    m_compat->SetForegroundColour(wxColour(120, 120, 120));
    root->Add(m_compat, 0, wxLEFT | wxRIGHT | wxBOTTOM, em);

    // Sign-in row: status text + a Sign in / Sign out button.
    auto* auth = new wxBoxSizer(wxHORIZONTAL);
    m_auth_status = new wxStaticText(this, wxID_ANY, "");
    m_auth_btn    = new wxButton(this, wxID_ANY, _L("Sign in"));
    auth->Add(m_auth_status, 1, wxALIGN_CENTER_VERTICAL);
    auth->Add(m_auth_btn, 0);
    root->Add(auth, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    m_lookup = new wxStaticText(this, wxID_ANY, "");
    m_lookup->SetForegroundColour(wxColour(120, 120, 120));
    root->Add(m_lookup, 0, wxLEFT | wxRIGHT | wxBOTTOM, em);

    m_queue = new wxCheckBox(this, wxID_ANY, _L("Queue on the selected printer after upload"));
    root->Add(m_queue, 0, wxLEFT | wxRIGHT | wxBOTTOM, em);

    m_status = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_status, 0, wxLEFT | wxRIGHT, em);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto* cancel = new wxButton(this, wxID_CANCEL, _L("Close"));
    m_send = new wxButton(this, wxID_ANY, _L("Send to Farm"));
    btns->Add(cancel, 0, wxRIGHT, em / 2);
    btns->Add(m_send, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, em);

    SetSizerAndFit(root);
    CenterOnParent();

    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { do_send(); });
    m_part_number->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { lookup_part(); e.Skip(); });
    m_part_number->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { lookup_part(); });
    m_farm_url->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) {
        if (auto* ac = wxGetApp().app_config) {
            wxString v = m_farm_url->GetValue(); v.Trim().Trim(false);
            ac->set("farm", "url", v.ToStdString());
        }
        e.Skip();
    });
    m_auth_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_auth_polling) {                         // waiting on the browser -> cancel
            stop_auth_poll();
            m_auth_status->SetLabel(_L("Not signed in"));
            Layout();
        } else if (!farm_token().empty()) {           // signed in -> sign out
            if (auto* ac = wxGetApp().app_config) ac->set("farm", "token", std::string());
            update_auth_ui();
        } else {
            do_sign_in();
        }
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll_auth(); });

    update_auth_ui();
    detect_slice_machine();    // know the plate's machine kind so we can filter the printer list
    load_opened_context();     // prefill part number from the opened project (sidecar/filename)
    fetch_printers();
    if (!m_have_opened_pn)     // only peek an auto number when this isn't a save-back of a known part
        fetch_next_part_number();
}

SendToFarmDialog::~SendToFarmDialog()
{
    if (m_alive) *m_alive = false;   // any in-flight CallAfter will now bail safely
    if (m_auth_timer) { m_auth_timer->Stop(); delete m_auth_timer; m_auth_timer = nullptr; }
}

// Peek the next auto part number from the farm and pre-fill the field (only if the
// user hasn't typed their own), so the sent G-code/3MF and the saved project all
// share the part number.
void SendToFarmDialog::fetch_next_part_number()
{
    auto http = Http::get(farm_url() + "/api/slicer/next-part-number?kind=custom");
    add_token(http, farm_token());
    http.timeout_connect(3).timeout_max(6)
        .on_error([this](std::string, std::string, unsigned status) {
            if (status != 401) return;
            wxGetApp().CallAfter([this, alive = m_alive]() { if (*alive) handle_unauthorized(); });
        })
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            std::string pn;
            try { pn = json::parse(body).value("part_number", std::string()); } catch (...) { return; }
            if (pn.empty()) return;
            wxGetApp().CallAfter([this, alive = m_alive, pn]() {
                if (!*alive) return;
                if (m_part_number->GetValue().Trim().Trim(false).IsEmpty()) {
                    m_part_number->SetValue(wxString::FromUTF8(pn));
                    m_lookup->SetLabel(wxString::Format(_L("New part — will be assigned %s"),
                                                        wxString::FromUTF8(pn)));
                    apply_part_number(pn);
                    Layout();
                }
            });
        })
        .perform();
}

// Make the part number the project's name so File -> Save Project defaults the
// .3mf filename to it.
void SendToFarmDialog::apply_part_number(const std::string& pn)
{
    if (pn.empty()) return;
    if (Plater* plater = wxGetApp().plater())
        plater->set_project_filename(wxString::FromUTF8(pn + ".3mf"));
}

std::string SendToFarmDialog::farm_url() const
{
    // FARM_URL env wins (dev override); then the live/persisted field; then the cloud default.
    if (const char* env = std::getenv("FARM_URL"); env != nullptr && *env != 0)
        return env;
    if (m_farm_url) {
        wxString v = m_farm_url->GetValue(); v.Trim().Trim(false);
        while (v.EndsWith("/")) v.RemoveLast();     // no trailing slash before "/api/slicer/…"
        if (!v.IsEmpty()) return v.ToStdString();
    }
    return kDefaultFarmUrl;
}

std::string SendToFarmDialog::farm_token() const
{
    if (auto* ac = wxGetApp().app_config) return ac->get("farm", "token");
    return std::string();
}

// Identify the machine the active plate is sliced for. kind ("bambu"|"klipper")
// matches the farm's Printer.kind column; model (printer_model) is display-only.
void SendToFarmDialog::detect_slice_machine()
{
    auto* pb = wxGetApp().preset_bundle;
    if (pb == nullptr) return;
    m_slice_kind = pb->is_bbl_vendor() ? "bambu" : "klipper";
    try {
        const DynamicPrintConfig& cfg = pb->printers.get_edited_preset().config;
        if (cfg.has("printer_model"))
            m_slice_model = cfg.opt_string("printer_model");
    } catch (...) { /* leave model blank -> fall back to kind name */ }
}

void SendToFarmDialog::fetch_printers()
{
    auto http = Http::get(farm_url() + "/api/slicer/printers");
    add_token(http, farm_token());
    http.timeout_connect(3).timeout_max(6)
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            struct P { int id; std::string name, kind; };
            std::vector<P> printers;
            try {
                json arr = json::parse(body);
                for (auto& p : arr)
                    printers.push_back({ p.value("id", 0), p.value("name", std::string("printer")),
                                         p.value("kind", std::string()) });
            } catch (...) { return; }
            wxGetApp().CallAfter([this, alive = m_alive, printers]() {
                if (!*alive) return;
                // Filter to the slice's machine only when BOTH sides expose a kind — the farm
                // returns per-printer kinds AND we detected the plate's kind. Older farms that
                // don't send "kind" -> show everything (graceful, no behaviour change).
                const bool farm_has_kind = std::any_of(printers.begin(), printers.end(),
                                                       [](const P& p) { return !p.kind.empty(); });
                const bool filtering = farm_has_kind && !m_slice_kind.empty();
                m_printer->Clear(); m_printer_ids.clear();
                int hidden = 0;
                for (const P& p : printers) {
                    if (filtering && !p.kind.empty() && p.kind != m_slice_kind) { ++hidden; continue; }
                    m_printer->Append(wxString::FromUTF8(p.name)); m_printer_ids.push_back(p.id);
                }
                if (!m_printer_ids.empty()) m_printer->SetSelection(0);
                if (m_compat != nullptr) {
                    const wxString mach = wxString::FromUTF8(m_slice_model.empty() ? m_slice_kind : m_slice_model);
                    if (!filtering) {
                        m_compat->SetLabel("");
                    } else if (m_printer_ids.empty()) {
                        m_compat->SetForegroundColour(wxColour(180, 60, 50));
                        m_compat->SetLabel(wxString::Format(_L("Sliced for %s — no matching printer on the farm."), mach));
                    } else {
                        m_compat->SetForegroundColour(wxColour(120, 120, 120));
                        wxString lbl = wxString::Format(_L("Sliced for %s — showing compatible printers"), mach);
                        if (hidden > 0) lbl += wxString::Format(_L(" (%d hidden)"), hidden);
                        m_compat->SetLabel(lbl);
                    }
                    Layout();
                }
            });
        })
        .on_error([this](std::string, std::string error, unsigned status) {
            wxGetApp().CallAfter([this, alive = m_alive, error, status]() {
                if (!*alive) return;
                if (status == 401) { handle_unauthorized(); return; }
                m_status->SetForegroundColour(wxColour(180, 60, 50));
                m_status->SetLabel(_L("Couldn't reach the farm at ") + farm_url() + " — is it running?");
                Layout();
            });
        })
        .perform();
}

void SendToFarmDialog::lookup_part()
{
    std::string pn = m_part_number->GetValue().Trim().Trim(false).ToStdString();
    if (pn.empty()) { m_lookup->SetLabel(_L("New part — number will be auto-assigned.")); Layout(); return; }
    auto http = Http::get(farm_url() + "/api/slicer/part-lookup?part_number=" + pn);
    add_token(http, farm_token());
    http.timeout_connect(3).timeout_max(6)
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            bool exists = false; std::string name, next_rev = "A";
            try { json j = json::parse(body); exists = j.value("exists", false);
                  name = j.value("name", std::string()); next_rev = j.value("next_rev_label", std::string("A")); }
            catch (...) { return; }
            wxGetApp().CallAfter([this, alive = m_alive, exists, name, next_rev]() {
                if (!*alive) return;
                // Show the next revision the farm will assign, but keep the field on "auto"
                // so the farm always allocates the next free label at send time.
                if (exists) m_lookup->SetLabel(wxString::Format(_L("Exists: %s — will add revision %s"),
                                                               wxString::FromUTF8(name), wxString::FromUTF8(next_rev)));
                else        m_lookup->SetLabel(_L("New part."));
                Layout();
            });
        })
        .on_error([this](std::string, std::string, unsigned status) {
            if (status != 401) return;
            wxGetApp().CallAfter([this, alive = m_alive]() { if (*alive) handle_unauthorized(); });
        })
        .perform();
}

// Pre-fill the opened project's part number so a save-back becomes its NEXT
// revision. Primary source: a "<3mf-path>.farmctx.json" sidecar the agent wrote
// next to the file it opened; fallback: the "<PN> - name.3mf" filename.
void SendToFarmDialog::load_opened_context()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return;
    const std::string proj = plater->get_project_filename(".3mf").ToStdString();
    if (proj.empty()) return;

    std::string pn, url;
    const fs::path sidecar(proj + ".farmctx.json");
    boost::system::error_code ec;
    bool from_sidecar = false;
    if (fs::exists(sidecar, ec)) {
        try {
            fs::ifstream f(sidecar);
            json j = json::parse(f);
            pn  = j.value("part_number", std::string());
            url = j.value("farm_url", std::string());
            from_sidecar = !pn.empty();
        } catch (...) { /* malformed sidecar — fall through to filename */ }
    }

    if (pn.empty()) {                                  // fallback: filename token before first " - "
        std::string stem = fs::path(proj).stem().string();
        auto dash = stem.find(" - ");
        std::string tok = (dash == std::string::npos) ? stem : stem.substr(0, dash);
        // trim
        while (!tok.empty() && std::isspace((unsigned char)tok.back())) tok.pop_back();
        while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
        if (looks_like_part_number(tok)) pn = tok;
    }

    if (pn.empty()) return;                            // nothing recognisable; leave for auto-peek

    m_have_opened_pn = true;
    if (!url.empty()) {
        m_farm_url->SetValue(wxString::FromUTF8(url));
        if (auto* ac = wxGetApp().app_config) ac->set("farm", "url", url);
    }
    m_part_number->SetValue(wxString::FromUTF8(pn));
    if (from_sidecar) fs::remove(sidecar, ec);         // consumed — a stale one would mis-tag a later, unrelated file
    lookup_part();                                     // confirm against the farm + show the next revision
}

void SendToFarmDialog::update_auth_ui()
{
    const std::string tok = farm_token();
    std::string acct;
    if (auto* ac = wxGetApp().app_config) acct = ac->get("farm", "account");
    if (!tok.empty()) {
        m_auth_status->SetForegroundColour(wxColour(30, 150, 80));
        m_auth_status->SetLabel(wxString::Format(_L("Signed in as %s"),
                                                 wxString::FromUTF8(acct.empty() ? "user" : acct)));
        m_auth_btn->SetLabel(_L("Sign out"));
    } else {
        m_auth_status->SetForegroundColour(wxColour(120, 120, 120));
        m_auth_status->SetLabel(_L("Not signed in"));
        m_auth_btn->SetLabel(_L("Sign in"));
    }
    Layout();
}

void SendToFarmDialog::handle_unauthorized()
{
    if (auto* ac = wxGetApp().app_config) ac->set("farm", "token", std::string());
    update_auth_ui();
    m_status->SetForegroundColour(wxColour(180, 60, 50));
    m_status->SetLabel(_L("Please sign in to the farm."));
    Layout();
}

// Browser sign-in: ask the farm for a one-time state + URL, open it in the user's
// default browser (where they may already be logged in -> one click), then poll
// until the browser authorises and hand back the token. No password ever touches
// the slicer; only the token + account are persisted.
void SendToFarmDialog::do_sign_in()
{
    m_auth_status->SetForegroundColour(wxColour(120, 120, 120));
    m_auth_status->SetLabel(_L("Opening browser…"));
    m_auth_btn->SetLabel(_L("Cancel"));
    Layout();

    auto http = Http::post(farm_url() + "/api/slicer/auth/start");
    http.timeout_connect(4).timeout_max(10)
        .on_complete([this](std::string body, unsigned) {
            std::string state, url;
            try { json j = json::parse(body); state = j.value("state", std::string());
                  url = j.value("authorize_url", std::string()); } catch (...) {}
            wxGetApp().CallAfter([this, alive = m_alive, state, url]() {
                if (!*alive) return;
                if (state.empty() || url.empty()) {
                    m_auth_btn->SetLabel(_L("Sign in"));
                    m_auth_status->SetLabel(_L("Sign-in unavailable — update the farm?"));
                    Layout(); return;
                }
                m_auth_state = state; m_auth_polls = 0; m_auth_polling = true;
                wxLaunchDefaultBrowser(wxString::FromUTF8(url));
                m_auth_status->SetLabel(_L("Waiting for browser sign-in…"));
                if (m_auth_timer == nullptr) m_auth_timer = new wxTimer(this);
                m_auth_timer->Start(2000);
                Layout();
            });
        })
        .on_error([this](std::string, std::string, unsigned) {
            wxGetApp().CallAfter([this, alive = m_alive]() {
                if (!*alive) return;
                m_auth_polling = false;
                m_auth_btn->SetLabel(_L("Sign in"));
                m_auth_status->SetLabel(_L("Couldn't reach the farm to sign in."));
                Layout();
            });
        })
        .perform();
}

void SendToFarmDialog::stop_auth_poll()
{
    m_auth_polling = false;
    m_auth_state.clear();
    if (m_auth_timer) m_auth_timer->Stop();
    update_auth_ui();     // restores the button to Sign in / Sign out
}

// One poll tick: ask the farm whether the browser has authorised this `state` yet.
void SendToFarmDialog::poll_auth()
{
    if (!m_auth_polling || m_auth_state.empty()) return;
    if (++m_auth_polls > 150) {   // ~5 min at 2s
        stop_auth_poll();
        m_auth_status->SetForegroundColour(wxColour(180, 60, 50));
        m_auth_status->SetLabel(_L("Sign-in timed out — try again.")); Layout();
        return;
    }
    const json b = { {"state", m_auth_state} };
    auto http = Http::post(farm_url() + "/api/slicer/auth/poll");
    http.header("Content-Type", "application/json").set_post_body(b.dump());
    http.timeout_connect(4).timeout_max(8)
        .on_complete([this](std::string body, unsigned) {
            std::string st, token, account;
            try { json j = json::parse(body); st = j.value("status", std::string());
                  token = j.value("token", std::string()); account = j.value("account", std::string()); }
            catch (...) { return; }
            wxGetApp().CallAfter([this, alive = m_alive, st, token, account]() {
                if (!*alive || !m_auth_polling) return;
                if (st == "ok") {
                    if (auto* ac = wxGetApp().app_config) {
                        ac->set("farm", "token", token);
                        ac->set("farm", "account", account);
                    }
                    stop_auth_poll();
                    m_status->SetForegroundColour(wxColour(30, 150, 80));
                    m_status->SetLabel(_L("Signed in."));
                    fetch_printers();     // retry now that we're authorised
                    Layout();
                } else if (st == "expired") {
                    stop_auth_poll();
                    m_auth_status->SetForegroundColour(wxColour(180, 60, 50));
                    m_auth_status->SetLabel(_L("Sign-in expired — try again.")); Layout();
                }
                // "pending": keep waiting for the next tick
            });
        })
        .perform();   // transient poll errors are ignored; the next tick retries
}

void SendToFarmDialog::do_send()
{
  try {
    // 1. The sliced G-code of the current plate.
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    std::string gcode_path;
    if (plate != nullptr) {
        GCodeProcessorResult* res = plate->get_slice_result();
        if (res != nullptr && !res->filename.empty()) gcode_path = res->filename;
        if (gcode_path.empty() || !fs::exists(gcode_path)) gcode_path = plate->get_tmp_gcode_path();
    }
    if (gcode_path.empty() || !fs::exists(gcode_path)) {
        m_status->SetForegroundColour(wxColour(180, 60, 50));
        m_status->SetLabel(_L("Slice the plate first — no G-code to send.")); Layout(); return;
    }

    // 1b. Bambu/BBL printers (e.g. H2D) print a sliced .gcode.3mf, not raw G-code. When the active
    //     printer is a Bambu machine, export the printable sliced 3MF and send THAT as the print file
    //     so the farm can forward it to the printer verbatim. Non-BBL farm printers (Klipper K2 etc.)
    //     keep getting the raw .gcode.
    const bool  is_bambu       = wxGetApp().preset_bundle && wxGetApp().preset_bundle->is_bbl_vendor();
    fs::path    printable_path = fs::path(gcode_path);
    std::string print_ext      = ".gcode";
    if (is_bambu) {
        fs::path sliced_3mf = fs::temp_directory_path() / "atn_send_to_farm_print.gcode.3mf";
        try {
            const int plate_idx = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
            wxGetApp().plater()->export_3mf(sliced_3mf,
                SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode, plate_idx);
            if (fs::exists(sliced_3mf) && fs::file_size(sliced_3mf) > 0) {
                printable_path = sliced_3mf;
                print_ext      = ".gcode.3mf";
            }
        } catch (...) { /* fall back to raw G-code below */ }
    }

    // 2. Export the project 3MF to a temp file (silent — no dialogs).
    fs::path tmp_3mf = fs::temp_directory_path() / "atn_send_to_farm.3mf";
    bool have_3mf = false;
    try {
        wxGetApp().plater()->export_3mf(tmp_3mf, SaveStrategy::Silence);
        have_3mf = fs::exists(tmp_3mf) && fs::file_size(tmp_3mf) > 0;
    } catch (...) { have_3mf = false; }

    const std::string pn    = m_part_number->GetValue().Trim().Trim(false).ToStdString();
    const std::string name  = m_name->GetValue().Trim().Trim(false).ToStdString();
    const std::string rev   = m_rev->GetValue().Trim().Trim(false).ToStdString();
    const bool        queue = m_queue->IsChecked();
    int printer_id = -1;
    if (int sel = m_printer->GetSelection(); sel != wxNOT_FOUND && sel < (int)m_printer_ids.size())
        printer_id = m_printer_ids[sel];
    if (queue && printer_id < 0) {
        m_status->SetForegroundColour(wxColour(180, 60, 50));
        m_status->SetLabel(_L("Pick a printer to queue on.")); Layout(); return;
    }

    m_send->Disable();
    m_status->SetForegroundColour(wxColour(120, 120, 120));
    m_status->SetLabel(_L("Sending…")); Layout();

    // Name the uploaded G-code and 3MF by the part number (falls back to the
    // original/generic names when no part number is set yet).
    const std::string gcode_name = pn.empty() ? printable_path.filename().string() : (pn + print_ext);
    const std::string model_name = pn.empty() ? std::string("project.3mf") : (pn + ".3mf");
    // Also make the part number the project's save name.
    apply_part_number(pn);

    auto http = Http::post(farm_url() + "/api/slicer/send");
    add_token(http, farm_token());
    http.form_add("part_number", pn)
        .form_add("name", name)
        .form_add("rev_label", rev.empty() ? "auto" : rev)
        .form_add("queue", queue ? "1" : "0");
    if (printer_id >= 0) http.form_add("printer_id", std::to_string(printer_id));
    http.form_add_file("gcode_file", printable_path, gcode_name);
    if (have_3mf) http.form_add_file("model_file", tmp_3mf, model_name);

    http.on_complete([this](std::string body, unsigned status) {
            std::string msg = "Sent to the farm.";
            std::string assigned_pn;
            try {
                json j = json::parse(body);
                if (j.contains("message")) msg = j["message"].get<std::string>();
                assigned_pn = j.value("part_number", std::string());
            } catch (...) {}
            wxGetApp().CallAfter([this, alive = m_alive, msg, assigned_pn]() {
              if (!*alive) return;
              try {
                // The farm is authoritative for the part number (e.g. if it was auto).
                if (!assigned_pn.empty()) {
                    m_part_number->SetValue(wxString::FromUTF8(assigned_pn));
                    apply_part_number(assigned_pn);   // set_project_filename() fires a GUI cascade
                }
                m_status->SetForegroundColour(wxColour(30, 150, 80));
                m_status->SetLabel(wxString::FromUTF8("\xE2\x9C\x93 " + msg));   // ✓
                m_send->Enable(); Layout();
                // ATN: record this dispatched job in the assistant's learning diary.
                if (Plater* plater = wxGetApp().plater())
                    plater->notify_atn_sent_to_farm();
              } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "ATN SendToFarm post-upload UI update threw: " << e.what();
                if (m_send) m_send->Enable();
              } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "ATN SendToFarm post-upload UI update threw (non-std)";
                if (m_send) m_send->Enable();
              }
            });
        })
        .on_error([this](std::string body, std::string error, unsigned status) {
            std::string msg = error.empty() ? "send failed" : error;
            try { json j = json::parse(body); if (j.contains("detail")) msg = j["detail"].is_string() ? j["detail"].get<std::string>() : j["detail"].dump(); } catch (...) {}
            wxGetApp().CallAfter([this, alive = m_alive, msg, status]() {
                if (!*alive) return;
                m_send->Enable();
                if (status == 401) { handle_unauthorized(); return; }
                m_status->SetForegroundColour(wxColour(180, 60, 50));
                m_status->SetLabel(wxString::Format(_L("Failed (%u): "), status) + wxString::FromUTF8(msg));
                Layout();
            });
        })
        .perform();
  } catch (const std::exception& e) {
      BOOST_LOG_TRIVIAL(error) << "ATN SendToFarm do_send exception: " << e.what();
      if (m_status) {
          m_status->SetForegroundColour(wxColour(180, 60, 50));
          m_status->SetLabel(wxString::FromUTF8(std::string("Send failed: ") + e.what()));
      }
      if (m_send) m_send->Enable();
      Layout();
  } catch (...) {
      BOOST_LOG_TRIVIAL(error) << "ATN SendToFarm do_send: unknown exception";
      if (m_send) m_send->Enable();
  }
}

} // namespace GUI
} // namespace Slic3r
