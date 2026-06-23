#include "SendToFarmDialog.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/button.h>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

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

    fetch_printers();
    fetch_next_part_number();
}

// Peek the next auto part number from the farm and pre-fill the field (only if the
// user hasn't typed their own), so the sent G-code/3MF and the saved project all
// share the part number.
void SendToFarmDialog::fetch_next_part_number()
{
    Http::get(farm_url() + "/api/slicer/next-part-number?kind=custom")
        .timeout_connect(3).timeout_max(6)
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            std::string pn;
            try { pn = json::parse(body).value("part_number", std::string()); } catch (...) { return; }
            if (pn.empty()) return;
            wxGetApp().CallAfter([this, pn]() {
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
    if (const char* env = std::getenv("FARM_URL"); env != nullptr && *env != 0)
        return env;
    return "http://127.0.0.1:8000";
}

void SendToFarmDialog::fetch_printers()
{
    Http::get(farm_url() + "/api/slicer/printers")
        .timeout_connect(3).timeout_max(6)
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            std::vector<std::pair<int, std::string>> printers;
            try {
                json arr = json::parse(body);
                for (auto& p : arr)
                    printers.emplace_back(p.value("id", 0), p.value("name", std::string("printer")));
            } catch (...) { return; }
            wxGetApp().CallAfter([this, printers]() {
                m_printer->Clear(); m_printer_ids.clear();
                for (auto& [id, name] : printers) { m_printer->Append(name); m_printer_ids.push_back(id); }
                if (!printers.empty()) m_printer->SetSelection(0);
            });
        })
        .on_error([this](std::string, std::string error, unsigned) {
            wxGetApp().CallAfter([this, error]() {
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
    Http::get(farm_url() + "/api/slicer/part-lookup?part_number=" + pn)
        .timeout_connect(3).timeout_max(6)
        .on_complete([this](std::string body, unsigned status) {
            if (status != 200) return;
            bool exists = false; std::string name, next_rev = "A";
            try { json j = json::parse(body); exists = j.value("exists", false);
                  name = j.value("name", std::string()); next_rev = j.value("next_rev_label", std::string("A")); }
            catch (...) { return; }
            wxGetApp().CallAfter([this, exists, name, next_rev]() {
                if (exists) m_lookup->SetLabel(wxString::Format(_L("Exists: %s — next revision %s"),
                                                               wxString::FromUTF8(name), wxString::FromUTF8(next_rev)));
                else        m_lookup->SetLabel(_L("New part."));
                wxString rv = m_rev->GetValue().Trim().Trim(false);
                if (rv.IsEmpty() || rv.Lower() == "auto") m_rev->SetValue(wxString::FromUTF8(next_rev));
                Layout();
            });
        })
        .perform();
}

void SendToFarmDialog::do_send()
{
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
            wxGetApp().CallAfter([this, msg, assigned_pn]() {
                // The farm is authoritative for the part number (e.g. if it was auto).
                if (!assigned_pn.empty()) {
                    m_part_number->SetValue(wxString::FromUTF8(assigned_pn));
                    apply_part_number(assigned_pn);
                }
                m_status->SetForegroundColour(wxColour(30, 150, 80));
                m_status->SetLabel(wxString::FromUTF8("\xE2\x9C\x93 " + msg));   // ✓
                m_send->Enable(); Layout();
            });
        })
        .on_error([this](std::string body, std::string error, unsigned status) {
            std::string msg = error.empty() ? "send failed" : error;
            try { json j = json::parse(body); if (j.contains("detail")) msg = j["detail"].is_string() ? j["detail"].get<std::string>() : j["detail"].dump(); } catch (...) {}
            wxGetApp().CallAfter([this, msg, status]() {
                m_status->SetForegroundColour(wxColour(180, 60, 50));
                m_status->SetLabel(wxString::Format(_L("Failed (%u): "), status) + wxString::FromUTF8(msg));
                m_send->Enable(); Layout();
            });
        })
        .perform();
}

} // namespace GUI
} // namespace Slic3r
