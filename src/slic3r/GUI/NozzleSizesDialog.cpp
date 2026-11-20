#include "NozzleSizesDialog.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <wx/panel.h>
#include <wx/sizer.h>

#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevNozzleRack.h"
#include "DeviceCore/DevNozzleSystem.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MachineNozzleSlots.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace {

double diameter_value(const std::string &d)
{
    try { return std::stod(d); } catch (...) { return 0.; }
}

std::vector<std::string> split_list(const std::string &list)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char c : list) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    for (auto &s : out) {
        const size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    }
    return out;
}

bool volume_compatible(NozzleVolumeType nozzle, NozzleVolumeType wanted)
{
    return nozzle == wanted || nozzle == nvtHybrid || wanted == nvtHybrid;
}

ComboBox *make_combo(wxWindow *parent, int width)
{
    auto *cb = new ComboBox(parent, wxID_ANY, wxString(""), wxDefaultPosition, wxSize(width, -1), 0, nullptr, wxCB_READONLY);
    cb->GetDropDown().SetUseContentWidth(true);
    return cb;
}

int combo_tag(const ComboBox *cb)
{
    const int sel = cb->GetSelection();
    return sel < 0 ? -1 : int(intptr_t(cb->GetClientData(sel)));
}

} // namespace

// ------------------------------------------------------------------------------------------------ static helpers

int NozzleSizesDialog::match_slot(const std::vector<std::vector<Slot>> &slots, int ams_slot, const std::string &diameter,
                                  NozzleVolumeType volume)
{
    std::vector<const Slot *> flat;
    for (const auto &ext : slots)
        for (const auto &s : ext)
            flat.push_back(&s);
    auto fits = [&](int i, bool check_flow) {
        return i >= 0 && i < int(flat.size()) && flat[i]->installed && flat[i]->diameter == diameter &&
               (!check_flow || volume_compatible(flat[i]->volume, volume));
    };
    if (fits(ams_slot, true))
        return ams_slot;
    for (int i = 0; i < int(flat.size()); ++i)
        if (fits(i, true))
            return i;
    for (int i = 0; i < int(flat.size()); ++i)
        if (fits(i, false))
            return i;
    return -1;
}

int NozzleSizesDialog::extruder_of_slot(const std::vector<std::vector<Slot>> &slots, int flat_slot)
{
    int base = 0;
    for (size_t e = 0; e < slots.size(); ++e) {
        if (flat_slot < base + int(slots[e].size()))
            return int(e);
        base += int(slots[e].size());
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ construction

NozzleSizesDialog::NozzleSizesDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Nozzle sizes"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    read_state();
    build();
    wxGetApp().UpdateDlgDarkUI(this);
}

void NozzleSizesDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

// What the machine offers, what the project says is fitted, what each filament prints with.
void NozzleSizesDialog::read_state()
{
    PresetBundle             &pb      = *wxGetApp().preset_bundle;
    const DynamicPrintConfig &printer = pb.printers.get_edited_preset().config;
    const DynamicPrintConfig &project = pb.project_config;
    const DynamicPrintConfig  full    = pb.full_config();

    m_mode                    = multi_nozzle_diameter_mode_enabled(project);
    m_single_ext_multi_nozzle = is_single_ext_multi_nozzle(full);

    // Every diameter this printer is sold in (one machine preset per variant), smallest first.
    m_printer_diameters = pb.printers.diameters_of_selected_printer();
    m_printer_diameters.erase(std::remove_if(m_printer_diameters.begin(), m_printer_diameters.end(),
                                             [](const std::string &d) { return diameter_value(d) <= 0.; }),
                              m_printer_diameters.end());
    std::sort(m_printer_diameters.begin(), m_printer_diameters.end(),
              [](const std::string &a, const std::string &b) { return diameter_value(a) < diameter_value(b); });
    if (m_printer_diameters.empty())
        m_printer_diameters = {"0.4"};

    const auto  *nd    = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t n_ext = (nd && !nd->values.empty()) ? nd->values.size() : 1;
    m_extruder_default_diameter.assign(n_ext, m_printer_diameters.front());
    for (size_t e = 0; e < n_ext; ++e)
        if (nd && e < nd->values.size() && nd->values[e] > 0.)
            m_extruder_default_diameter[e] = format_diameter_to_str(nd->values[e]);

    // Flow types each extruder is sold with: extruder_variant_list[e] is "Direct Drive Standard,Direct Drive
    // High Flow,Direct Drive TPU285" — strip the extruder type and look the rest up in the enum.
    const auto *variants  = printer.option<ConfigOptionStrings>("extruder_variant_list");
    const auto *etype     = printer.option<ConfigOptionEnumsGeneric>("extruder_type");
    const auto *etype_def = print_config_def.get("extruder_type");
    const auto *nvt_def   = print_config_def.get("nozzle_volume_type");
    m_offered.assign(n_ext, {nvtStandard});
    for (size_t e = 0; e < n_ext; ++e) {
        if (!variants || e >= variants->values.size() || !nvt_def || !nvt_def->enum_keys_map)
            continue;
        std::string prefix;
        if (etype && etype_def && e < etype->values.size() && etype->values[e] >= 0 &&
            etype->values[e] < int(etype_def->enum_labels.size()))
            prefix = etype_def->enum_labels[etype->values[e]] + " ";
        std::vector<NozzleVolumeType> out;
        for (std::string token : split_list(variants->values[e])) {
            if (!prefix.empty() && token.compare(0, prefix.size(), prefix) == 0)
                token = token.substr(prefix.size());
            for (size_t j = 0; j < nvt_def->enum_values.size(); ++j) {
                const std::string &key   = nvt_def->enum_values[j];
                const std::string  label = j < nvt_def->enum_labels.size() ? nvt_def->enum_labels[j] : key;
                if (token != key && token != label)
                    continue;
                auto it = nvt_def->enum_keys_map->find(key);
                if (it == nvt_def->enum_keys_map->end())
                    break;
                const auto v = NozzleVolumeType(it->second);
                if (v != nvtHybrid && std::find(out.begin(), out.end(), v) == out.end())
                    out.push_back(v);
                break;
            }
        }
        if (!out.empty())
            m_offered[e] = out;
    }

    // Slots per extruder: the rack size from the machine, every slot fitted with the variant's diameter and the
    // extruder's current flow type …
    const auto *maxc = printer.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    const auto *nvt  = project.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    m_slots.assign(n_ext, {});
    for (size_t e = 0; e < n_ext; ++e) {
        int n_slots = 1;
        if (maxc && e < maxc->values.size() && maxc->values[e] != ConfigOptionIntsNullable::nil_value() && maxc->values[e] > 1)
            n_slots = maxc->values[e];
        Slot def;
        def.diameter  = m_extruder_default_diameter[e];
        def.installed = true;
        if (nvt && e < nvt->values.size() && NozzleVolumeType(nvt->values[e]) != nvtHybrid)
            def.volume = NozzleVolumeType(nvt->values[e]);
        if (std::find(m_offered[e].begin(), m_offered[e].end(), def.volume) == m_offered[e].end())
            def.volume = m_offered[e].front();
        m_slots[e].assign(size_t(n_slots), def);
    }
    // … overlaid with the project's own per-slot inventory when it describes this machine (same acceptance
    // rules as build_nozzle_list_from_extruder_nozzles: one count per extruder, flat lists covering the total).
    const auto *counts = project.option<ConfigOptionInts>("extruder_nozzle_count");
    const auto *vtypes = project.option<ConfigOptionEnumsGeneric>("extruder_nozzle_volume_type");
    const auto *diams  = project.option<ConfigOptionFloats>("extruder_nozzle_diameter");
    const auto *ids    = project.option<ConfigOptionInts>("extruder_nozzle_slot");
    if (counts && counts->values.size() == n_ext) {
        int total = 0;
        for (int c : counts->values)
            total += std::max(0, c);
        if (total > 0 && vtypes && int(vtypes->values.size()) == total) {
            const bool has_d = diams && int(diams->values.size()) == total;
            const bool has_s = ids && int(ids->values.size()) == total;
            for (auto &ext : m_slots)
                for (auto &s : ext)
                    s.installed = false;
            size_t vi = 0;
            int    flat_base = 0;
            for (size_t e = 0; e < n_ext; ++e) {
                for (int k = 0; k < counts->values[e]; ++k, ++vi) {
                    const int local = has_s ? ids->values[vi] - flat_base : k;
                    if (local >= 0 && local < int(m_slots[e].size())) {
                        Slot &s     = m_slots[e][local];
                        s.installed = true;
                        s.volume    = NozzleVolumeType(vtypes->values[vi]);
                        s.diameter  = has_d && diams->values[vi] > 0. ? format_diameter_to_str(diams->values[vi]) : m_extruder_default_diameter[e];
                        if (std::find(m_printer_diameters.begin(), m_printer_diameters.end(), s.diameter) == m_printer_diameters.end())
                            s.diameter = m_extruder_default_diameter[e];
                    }
                }
                flat_base += int(m_slots[e].size());
            }
            // A rack with nothing fitted is not a state anyone meant: back to the default.
            for (auto &ext : m_slots)
                if (std::none_of(ext.begin(), ext.end(), [](const Slot &s) { return s.installed; }))
                    for (auto &s : ext)
                        s.installed = true;
        }
    }

    // The slot the AMS feeds, on a rack (H3C: ams_nozzle_idx = 0). On a two-extruder machine the AMS mapping
    // is per extruder and no single slot is "the AMS slot".
    m_ams_slot = -1;
    if (m_single_ext_multi_nozzle) {
        if (const auto *ams = printer.option<ConfigOptionInts>("ams_nozzle_idx"); ams && !ams->values.empty())
            m_ams_slot = ams->values.front();
        if (!m_slots.empty() && (m_ams_slot < 0 || m_ams_slot >= int(m_slots.front().size())))
            m_ams_slot = -1;
    }

    // Filaments.
    const size_t n_fil = pb.filament_presets.size();
    const auto  *fdm   = project.option<ConfigOptionStrings>("filament_diameter_map");
    const auto  *fvm   = project.option<ConfigOptionInts>("filament_volume_map");
    const auto  *col   = project.option<ConfigOptionStrings>("filament_colour");
    m_fil_name.assign(n_fil, "");
    m_fil_colour.assign(n_fil, "#808080");
    m_fil_diameter.assign(n_fil, "");
    m_fil_volume.assign(n_fil, nvtStandard);
    for (size_t i = 0; i < n_fil; ++i) {
        const std::string &pname  = pb.filament_presets[i];
        const Preset      *preset = pb.filaments.find_preset(pname, false);
        m_fil_name[i] = preset ? (preset->alias.empty() ? preset->name : preset->alias) : pname;
        if (col && i < col->values.size() && !col->values[i].empty())
            m_fil_colour[i] = col->values[i];
        if (fdm && i < fdm->values.size())
            m_fil_diameter[i] = fdm->values[i];
        if (fvm && i < fvm->values.size())
            m_fil_volume[i] = NozzleVolumeType(fvm->values[i]);
    }
}

void NozzleSizesDialog::build()
{
    auto *main = new wxBoxSizer(wxVERTICAL);
    auto *body = new wxBoxSizer(wxVERTICAL);

    // The mode switch.
    auto *mode_row = new wxBoxSizer(wxHORIZONTAL);
    m_mode_check   = new ::CheckBox(this);   // the Widgets/ toggle; Slic3r::GUI has an unrelated CheckBox too
    m_mode_check->SetValue(m_mode);
    m_mode_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &) { on_mode_toggled(); });
    mode_row->Add(m_mode_check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    mode_row->Add(new Label(this, Label::Body_14, _L("Mixed nozzle sizes in this project")), 0, wxALIGN_CENTER_VERTICAL);
    mode_row->AddStretchSpacer(1);
    m_read_btn = new Button(this, _L("Read from printer"));
    m_read_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_read_btn->SetToolTip(_L("Fill the list below with the nozzles the connected printer reports"));
    m_read_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { read_from_printer(); });
    mode_row->Add(m_read_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
    body->Add(mode_row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_hint = new Label(this, Label::Body_12,
                       _L("Each filament is assigned the nozzle it prints with; layer heights, line widths, walls and "
                          "retraction follow that nozzle. Record the nozzles fitted below first, then pick one per "
                          "filament. Saved with the project."));
    m_hint->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    m_hint->Wrap(FromDIP(540));
    body->Add(m_hint, 0, wxBOTTOM, FromDIP(12));
    // What the last "Read from printer" did; hidden until the button is used.
    m_read_status = new Label(this, Label::Body_12, wxString(""));
    body->Add(m_read_status, 0, wxBOTTOM, FromDIP(12));
    m_read_status->Hide();

    // What is fitted, extruder by extruder.
    const size_t n_ext = m_slots.size();
    m_slot_rows.assign(n_ext, {});
    int flat = 0;
    for (size_t e = 0; e < n_ext; ++e) {
        wxString title;
        if (n_ext == 1)
            title = m_slots[e].size() > 1 ? _L("Hotend rack") : _L("Extruder");
        else
            title = e == 0 ? _L("Left extruder") : (n_ext == 2 ? _L("Right extruder") : wxString::Format(_L("Extruder %d"), int(e + 1)));
        body->Add(new Label(this, Label::Body_13.Bold(), title), 0, wxTOP | wxBOTTOM, FromDIP(4));

        auto *grid = new wxFlexGridSizer(0, 3, FromDIP(4), FromDIP(10));
        for (size_t k = 0; k < m_slots[e].size(); ++k, ++flat) {
            wxString name = m_slots[e].size() > 1 ? wxString::Format(_L("Slot %d"), int(k + 1)) : _L("Nozzle");
            if (flat == m_ams_slot)
                name += " " + _L("(AMS)");
            grid->Add(new Label(this, Label::Body_13, name), 0, wxALIGN_CENTER_VERTICAL);
            SlotRow row;
            row.diameter = make_combo(this, FromDIP(110));
            row.flow     = make_combo(this, FromDIP(160));
            fill_slot_diameter(e, k, row);
            fill_slot_flow(e, k, row);
            row.diameter->Bind(wxEVT_COMBOBOX, [this, e, k](wxCommandEvent &) { on_slot_diameter(e, k); });
            row.flow->Bind(wxEVT_COMBOBOX, [this, e, k](wxCommandEvent &) { on_slot_flow(e, k); });
            grid->Add(row.diameter, 0);
            grid->Add(row.flow, 0);
            m_slot_rows[e].push_back(row);
        }
        body->Add(grid, 0, wxLEFT | wxBOTTOM, FromDIP(8));
    }

    // Which nozzle each filament prints with.
    body->Add(new Label(this, Label::Body_13.Bold(), _L("Filaments")), 0, wxTOP | wxBOTTOM, FromDIP(4));
    m_fil_panel = new wxPanel(this);
    m_fil_panel->SetBackgroundColour(*wxWHITE);
    m_fil_sizer = new wxFlexGridSizer(0, 3, FromDIP(4), FromDIP(10));
    m_fil_panel->SetSizer(m_fil_sizer);
    body->Add(m_fil_panel, 0, wxLEFT | wxBOTTOM, FromDIP(8));
    rebuild_filament_rows();

    main->Add(body, 1, wxEXPAND | wxALL, FromDIP(20));

    auto *btns = new DialogButtons(this, {"OK", "Cancel"});
    btns->GetOK()->SetLabel(_L("Apply"));
    btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxString why;
        if (!validate(why)) {
            MessageDialog dlg(this, why, _L("Nozzle sizes"), wxOK | wxICON_WARNING);
            dlg.ShowModal();
            return;
        }
        apply();
        EndModal(wxID_OK);
    });
    main->Add(btns, 0, wxEXPAND);

    SetSizer(main);
    on_mode_toggled();
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();
}

// ------------------------------------------------------------------------------------------------ rows

void NozzleSizesDialog::fill_slot_diameter(size_t e, size_t k, SlotRow &row)
{
    const Slot &slot = m_slots[e][k];
    row.diameter->Clear();
    int sel = -1;
    for (size_t i = 0; i < m_printer_diameters.size(); ++i) {
        row.diameter->Append(wxString::FromUTF8(m_printer_diameters[i]) + " mm", wxNullBitmap, (void *) (intptr_t) i);
        if (slot.installed && slot.diameter == m_printer_diameters[i])
            sel = int(i);
    }
    if (m_slots[e].size() > 1) { // a rack slot can be empty; a lone nozzle cannot
        row.diameter->Append(_L("empty"), wxNullBitmap, (void *) (intptr_t) -1);
        if (!slot.installed)
            sel = int(m_printer_diameters.size());
    }
    row.diameter->SetSelection(std::max(sel, 0));
}

void NozzleSizesDialog::fill_slot_flow(size_t e, size_t k, SlotRow &row)
{
    Slot &slot = m_slots[e][k];
    row.flow->Clear();
    int sel = -1, n = 0;
    for (NozzleVolumeType v : m_offered[e]) {
        if (!nozzle_variant_exists(slot.diameter, v))
            continue;
        row.flow->Append(volume_label(v), wxNullBitmap, (void *) (intptr_t) v);
        if (v == slot.volume)
            sel = n;
        ++n;
    }
    if (sel < 0 && n > 0) {
        sel         = 0;
        slot.volume = NozzleVolumeType(intptr_t(row.flow->GetClientData(0)));
    }
    if (sel >= 0)
        row.flow->SetSelection(sel);
    row.flow->Enable(slot.installed && n > 0);
}

void NozzleSizesDialog::fill_filament_flow(size_t i, FilRow &row)
{
    row.flow->Clear();
    const auto vols = installed_volumes_for(m_fil_diameter[i]);
    if (std::find(vols.begin(), vols.end(), m_fil_volume[i]) == vols.end() && !vols.empty())
        m_fil_volume[i] = vols.front();
    int n = 0;
    for (NozzleVolumeType v : vols) {
        row.flow->Append(volume_label(v), wxNullBitmap, (void *) (intptr_t) v);
        if (v == m_fil_volume[i])
            row.flow->SetSelection(n);
        ++n;
    }
    row.flow->Enable(m_mode && n > 0);
}

void NozzleSizesDialog::on_slot_diameter(size_t e, size_t k)
{
    SlotRow  &row  = m_slot_rows[e][k];
    Slot     &slot = m_slots[e][k];
    const int tag  = combo_tag(row.diameter);
    if (tag < 0 || tag >= int(m_printer_diameters.size())) {
        slot.installed = false;
    } else {
        slot.installed = true;
        slot.diameter  = m_printer_diameters[tag];
    }
    fill_slot_flow(e, k, row);
    rebuild_filament_rows();
}

void NozzleSizesDialog::on_slot_flow(size_t e, size_t k)
{
    const int tag = combo_tag(m_slot_rows[e][k].flow);
    if (tag >= 0)
        m_slots[e][k].volume = NozzleVolumeType(tag);
    rebuild_filament_rows();
}

void NozzleSizesDialog::on_mode_toggled()
{
    m_mode = m_mode_check->GetValue();
    for (auto &ext : m_slot_rows)
        for (auto &row : ext)
            row.diameter->Enable(m_mode);
    rebuild_filament_rows();
}

void NozzleSizesDialog::rebuild_filament_rows()
{
    if (!m_fil_sizer)
        return;
    m_fil_sizer->Clear(true);
    m_fil_rows.clear();

    const auto diameters = installed_diameters();
    for (size_t i = 0; i < m_fil_name.size(); ++i) {
        // Keep every pick valid against what is fitted: a filament whose nozzle was removed moves to the AMS
        // slot's diameter, else the first fitted one.
        if (std::find(diameters.begin(), diameters.end(), m_fil_diameter[i]) == diameters.end()) {
            std::string d;
            if (const Slot *ams = slot_at(m_ams_slot); ams && ams->installed)
                d = ams->diameter;
            if (d.empty() && !diameters.empty())
                d = diameters.front();
            m_fil_diameter[i] = d;
        }

        auto *name_row = new wxBoxSizer(wxHORIZONTAL);
        auto *swatch   = new wxPanel(m_fil_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(14), FromDIP(14)));
        swatch->SetBackgroundColour(wxColour(wxString::FromUTF8(m_fil_colour[i])));
        name_row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        name_row->Add(new Label(m_fil_panel, Label::Body_13, wxString::Format("%d  ", int(i + 1)) + wxString::FromUTF8(m_fil_name[i])),
                      0, wxALIGN_CENTER_VERTICAL);
        m_fil_sizer->Add(name_row, 0, wxALIGN_CENTER_VERTICAL);

        FilRow row;
        row.diameter = make_combo(m_fil_panel, FromDIP(110));
        for (size_t j = 0; j < diameters.size(); ++j) {
            row.diameter->Append(wxString::FromUTF8(diameters[j]) + " mm", wxNullBitmap, (void *) (intptr_t) j);
            if (diameters[j] == m_fil_diameter[i])
                row.diameter->SetSelection(int(j));
        }
        row.diameter->Enable(m_mode && !diameters.empty());
        row.flow = make_combo(m_fil_panel, FromDIP(160));
        fill_filament_flow(i, row);

        row.diameter->Bind(wxEVT_COMBOBOX, [this, i, diameters](wxCommandEvent &) {
            const int j = combo_tag(m_fil_rows[i].diameter);
            if (j >= 0 && j < int(diameters.size()))
                m_fil_diameter[i] = diameters[j];
            fill_filament_flow(i, m_fil_rows[i]);
        });
        row.flow->Bind(wxEVT_COMBOBOX, [this, i](wxCommandEvent &) {
            const int tag = combo_tag(m_fil_rows[i].flow);
            if (tag >= 0)
                m_fil_volume[i] = NozzleVolumeType(tag);
        });

        m_fil_sizer->Add(row.diameter, 0);
        m_fil_sizer->Add(row.flow, 0);
        m_fil_rows.push_back(row);
    }
    m_fil_panel->Layout();
    Layout();
    Fit();
}

// ------------------------------------------------------------------------------------------------ read from printer

void NozzleSizesDialog::set_read_status(const wxString &text, bool warning)
{
    m_read_status->SetLabel(text);
    m_read_status->SetForegroundColour(StateColor::darkModeColorFor(wxColour(warning ? "#C2540A" : "#6B6B6B")));
    m_read_status->Wrap(FromDIP(540));
    m_read_status->Show();
    Layout();
    Fit();
}

// Fill the slots from the nozzle report of the connected printer. Nothing is written to the project here:
// the rows change, the user checks them and Apply writes as usual.
void NozzleSizesDialog::read_from_printer()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    if (!obj || !obj->is_connected()) {
        set_read_status(_L("No printer is connected. Open the Device tab, connect to the printer, then press Read from "
                           "printer again."), true);
        return;
    }
    PresetBundle         &pb        = *wxGetApp().preset_bundle;
    const wxString        name      = wxString::FromUTF8(obj->get_dev_name());
    const DevNozzleSystem *sys      = obj->GetNozzleSystem();
    const DevExtderSystem *exts     = obj->GetExtderSystem();
    const int             total_ext = exts ? exts->GetTotalExtderCount() : 1;
    if (pb.printers.get_edited_preset().get_printer_type(&pb) != obj->get_show_printer_type() || total_ext != int(m_slots.size())) {
        set_read_status(wxString::Format(_L("%s is not the printer model selected in this project, so its nozzles were not "
                                            "read. Select its printer preset first."), name), true);
        return;
    }
    if (!sys || (sys->GetExtNozzles().empty() && sys->GetRackNozzles().empty())) {
        set_read_status(wxString::Format(_L("%s has not reported its nozzles yet. Wait a few seconds and press Read from "
                                            "printer again."), name), true);
        return;
    }

    // The printer's tools, reduced to what the placement needs. Health is only judged on rack printers, as
    // Bambu Studio does: a single fixed nozzle reports no such state.
    const auto rack_dev = sys->GetNozzleRack();
    const bool has_rack = rack_dev && rack_dev->IsSupported();
    auto reported = [&](const DevNozzle &n) {
        MachineNozzleSlots::Reported r;
        r.on_rack  = n.IsOnRack();
        r.id       = n.GetNozzleId();
        r.addr_id  = n.GetAddrId().value_or(-1);
        // Two extruders: the rack and the main nozzle are the right one (index 1), the deputy nozzle the left.
        r.extruder = total_ext < 2 ? 0 : ((r.on_rack || r.id == MAIN_EXTRUDER_ID) ? 1 : 0);
        r.exists   = n.IsExist();
        r.usable   = n.IsHotend() && (!has_rack || (n.IsNormal() && !n.IsUnknown()));
        // format_diameter_to_str snaps to the nearest size, so only a value that IS one of them may pass.
        const double d = n.GetNozzleDiameter();
        if (d > 0.05) {
            const std::string s = format_diameter_to_str(d);
            if (std::abs(diameter_value(s) - d) < 0.05)
                r.diameter = s;
        }
        r.volume = int(DevNozzle::ToNozzleVolumeType(n.GetNozzleFlowType()));
        return r;
    };
    std::vector<MachineNozzleSlots::Reported> tools;
    for (const auto &item : sys->GetRackNozzles())
        tools.push_back(reported(item.second));
    for (const auto &item : sys->GetExtNozzles())
        tools.push_back(reported(item.second));

    using MachineNozzleSlots::SlotRead;
    auto next = m_slots;
    int  read = 0, emptied = 0, unidentified = 0, reflowed = 0;
    for (size_t e = 0; e < next.size(); ++e) {
        const auto found   = MachineNozzleSlots::place(tools, int(e), int(next[e].size()));
        const bool is_rack = next[e].size() > 1;
        for (size_t k = 0; k < found.size(); ++k) {
            Slot      &slot   = next[e][k];
            const bool nozzle = found[k].state == SlotRead::Nozzle &&
                                std::find(m_printer_diameters.begin(), m_printer_diameters.end(), found[k].diameter) != m_printer_diameters.end();
            if (nozzle) {
                slot.installed = true;
                slot.diameter  = found[k].diameter;
                slot.volume    = NozzleVolumeType(found[k].volume);
                // A flow type this printer preset has no variant for cannot be sliced as such: the row falls
                // back to one it has (fill_slot_flow), and the status line says so.
                if (std::find(m_offered[e].begin(), m_offered[e].end(), slot.volume) == m_offered[e].end() ||
                    !nozzle_variant_exists(slot.diameter, slot.volume))
                    ++reflowed;
                ++read;
                continue;
            }
            if (found[k].state != SlotRead::Vacant)
                ++unidentified;
            if (is_rack) { // a lone nozzle cannot be empty: it keeps what the project says
                slot.installed = false;
                ++emptied;
            }
        }
    }
    if (read == 0) {
        set_read_status(wxString::Format(_L("%s did not report a nozzle that can be used here, so nothing was changed. If it "
                                            "is still reading its hotends, press Read from printer again when it has "
                                            "finished."), name), true);
        return;
    }

    m_slots = next;
    for (size_t e = 0; e < m_slots.size(); ++e)
        for (size_t k = 0; k < m_slots[e].size() && k < m_slot_rows[e].size(); ++k) {
            fill_slot_diameter(e, k, m_slot_rows[e][k]);
            fill_slot_flow(e, k, m_slot_rows[e][k]);
        }
    // More than one size on the printer is what the mode is for: off, Apply would write every slot at the
    // preset's diameter and what was just read would be lost.
    bool switched_on = false;
    if (!m_mode && installed_diameters().size() > 1) {
        m_mode_check->SetValue(true);
        switched_on = true;
    }
    on_mode_toggled(); // also re-seats every filament on a nozzle that is fitted now

    wxString msg = wxString::Format(_L_PLURAL("Read %d nozzle from %s.", "Read %d nozzles from %s.", read), read, name);
    if (emptied > 0)
        msg += " " + wxString::Format(_L_PLURAL("%d slot is empty.", "%d slots are empty.", emptied), emptied);
    if (unidentified > 0)
        msg += " " + wxString::Format(_L_PLURAL("%d tool is not one the printer has identified as a usable nozzle and was left out.",
                                                "%d tools are not ones the printer has identified as usable nozzles and were left out.",
                                                unidentified), unidentified);
    if (reflowed > 0)
        msg += " " + wxString::Format(_L_PLURAL("%d nozzle has a flow type this printer preset has no profile for; check its row.",
                                                "%d nozzles have a flow type this printer preset has no profile for; check their rows.",
                                                reflowed), reflowed);
    if (sys->IsRefreshing())
        msg += " " + _L("The printer is still reading its hotends, so this may be incomplete.");
    if (switched_on)
        msg += " " + _L("The printer carries more than one nozzle size, so mixed nozzle sizes was switched on.");
    bool warn = unidentified > 0 || reflowed > 0 || sys->IsRefreshing();
    if (!m_mode) {
        // One size throughout, but not the size of the selected printer preset.
        for (size_t e = 0; e < m_slots.size(); ++e) {
            const auto it = std::find_if(m_slots[e].begin(), m_slots[e].end(),
                                         [&](const Slot &s) { return s.installed && s.diameter != m_extruder_default_diameter[e]; });
            if (it == m_slots[e].end())
                continue;
            msg += " " + wxString::Format(_L("The nozzles are %s mm but the selected printer preset is the %s mm one: select the "
                                             "%s mm printer preset to slice for them."),
                                          wxString::FromUTF8(it->diameter), wxString::FromUTF8(m_extruder_default_diameter[e]),
                                          wxString::FromUTF8(it->diameter));
            warn = true;
            break;
        }
    }
    msg += " " + _L("Check the list, then press Apply.");
    set_read_status(msg, warn);
}

// ------------------------------------------------------------------------------------------------ queries

std::vector<std::string> NozzleSizesDialog::installed_diameters() const
{
    std::set<std::string> set;
    for (const auto &ext : m_slots)
        for (const auto &s : ext)
            if (s.installed)
                set.insert(s.diameter);
    std::vector<std::string> out(set.begin(), set.end());
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return diameter_value(a) < diameter_value(b); });
    return out;
}

std::vector<NozzleVolumeType> NozzleSizesDialog::installed_volumes_for(const std::string &diameter) const
{
    std::set<int> set;
    for (const auto &ext : m_slots)
        for (const auto &s : ext)
            if (s.installed && s.diameter == diameter)
                set.insert(int(s.volume));
    std::vector<NozzleVolumeType> out;
    for (int v : set)
        out.push_back(NozzleVolumeType(v));
    return out;
}

const NozzleSizesDialog::Slot *NozzleSizesDialog::slot_at(int flat_slot) const
{
    if (flat_slot < 0)
        return nullptr;
    int base = 0;
    for (const auto &ext : m_slots) {
        if (flat_slot < base + int(ext.size()))
            return &ext[size_t(flat_slot - base)];
        base += int(ext.size());
    }
    return nullptr;
}

wxString NozzleSizesDialog::volume_label(NozzleVolumeType v) const
{
    if (const auto *def = print_config_def.get("nozzle_volume_type"); def && def->enum_keys_map) {
        for (size_t j = 0; j < def->enum_values.size(); ++j) {
            auto it = def->enum_keys_map->find(def->enum_values[j]);
            if (it != def->enum_keys_map->end() && it->second == int(v))
                return _L(j < def->enum_labels.size() ? def->enum_labels[j] : def->enum_values[j]);
        }
    }
    return wxString::Format("%d", int(v));
}

bool NozzleSizesDialog::validate(wxString &why) const
{
    for (size_t e = 0; e < m_slots.size(); ++e)
        if (std::none_of(m_slots[e].begin(), m_slots[e].end(), [](const Slot &s) { return s.installed; })) {
            why = _L("Every extruder needs at least one nozzle fitted.");
            return false;
        }
    if (m_mode) {
        const auto diameters = installed_diameters();
        for (size_t i = 0; i < m_fil_diameter.size(); ++i) {
            if (std::find(diameters.begin(), diameters.end(), m_fil_diameter[i]) == diameters.end()) {
                why = wxString::Format(_L("Filament %d is set to a %s mm nozzle, but no such nozzle is fitted."), int(i + 1),
                                       wxString::FromUTF8(m_fil_diameter[i]));
                return false;
            }
            const auto vols = installed_volumes_for(m_fil_diameter[i]);
            if (std::find(vols.begin(), vols.end(), m_fil_volume[i]) == vols.end()) {
                why = wxString::Format(_L("Filament %d is set to a %s mm %s nozzle, but none is fitted."), int(i + 1),
                                       wxString::FromUTF8(m_fil_diameter[i]), volume_label(m_fil_volume[i]));
                return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ apply

void NozzleSizesDialog::apply()
{
    PresetBundle       &pb      = *wxGetApp().preset_bundle;
    Plater             *plater  = wxGetApp().plater();
    DynamicPrintConfig &project = pb.project_config;
    const size_t        n_ext   = m_slots.size();
    const size_t        n_fil   = pb.filament_presets.size();

    project.option<ConfigOptionBool>("multi_nozzle_diameter_mode", true)->value = m_mode;

    // The per-slot inventory, in the shape build_nozzle_list_from_extruder_nozzles reads: one count per
    // extruder, flat lists over the FITTED slots, and the physical slot id of each (flat across extruders)
    // so an empty slot keeps its place. With the mode off every slot carries the machine variant's diameter.
    std::vector<int>    counts(n_ext, 0), vtypes, slot_ids, ext_volume(n_ext, int(nvtStandard));
    std::vector<double> diams;
    int                 flat_base = 0;
    for (size_t e = 0; e < n_ext; ++e) {
        std::set<int> kinds;
        for (size_t k = 0; k < m_slots[e].size(); ++k) {
            const Slot &s = m_slots[e][k];
            if (!s.installed)
                continue;
            ++counts[e];
            diams.push_back(diameter_value(m_mode ? s.diameter : m_extruder_default_diameter[e]));
            vtypes.push_back(int(s.volume));
            slot_ids.push_back(flat_base + int(k));
            kinds.insert(int(s.volume));
        }
        // The extruder's own flow type: what its nozzles are, or Hybrid for a rack holding a mix.
        if (kinds.size() == 1)
            ext_volume[e] = *kinds.begin();
        else if (kinds.size() > 1)
            ext_volume[e] = int(nvtHybrid);
        flat_base += int(m_slots[e].size());
    }
    project.option<ConfigOptionInts>("extruder_nozzle_count", true)->values               = counts;
    project.option<ConfigOptionFloats>("extruder_nozzle_diameter", true)->values           = diams;
    project.option<ConfigOptionEnumsGeneric>("extruder_nozzle_volume_type", true)->values = vtypes;
    project.option<ConfigOptionInts>("extruder_nozzle_slot", true)->values                = slot_ids;

    // Per-extruder flow type through the same path the sidebar's flow combo takes (it also re-derives the
    // per-filament volumes of that extruder; those are rewritten from the picks right after).
    if (auto *nvt = project.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")) {
        auto *printer_tab = dynamic_cast<TabPrinter *>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
        for (size_t e = 0; e < n_ext && e < nvt->values.size(); ++e) {
            if (nvt->values[e] == ext_volume[e])
                continue;
            if (printer_tab)
                printer_tab->set_extruder_volume_type(int(e), NozzleVolumeType(ext_volume[e]));
            if (plater)
                plater->update_filament_volume_map(int(e), ext_volume[e]);
        }
    }

    // What each filament prints with.
    auto *fdm = project.option<ConfigOptionStrings>("filament_diameter_map", true);
    auto *fvm = project.option<ConfigOptionInts>("filament_volume_map", true);
    fdm->values.resize(n_fil);
    fvm->values.resize(n_fil, int(nvtStandard));
    for (size_t i = 0; i < n_fil; ++i) {
        fdm->values[i] = (m_mode && i < m_fil_diameter.size()) ? m_fil_diameter[i] : std::string();
        if (i < m_fil_volume.size())
            fvm->values[i] = int(m_fil_volume[i]);
    }
    PartPlateList &plates = plater->get_partplate_list();
    for (PartPlate *plate : plates.get_plate_list()) {
        if (!plate)
            continue;
        std::vector<int> v = plate->get_filament_volume_maps();
        if (v.empty())
            continue; // no per-plate copy: it follows the project values
        v.resize(n_fil, int(nvtStandard));
        for (size_t i = 0; i < n_fil; ++i)
            v[i] = fvm->values[i];
        plate->set_filament_volume_maps(v);
    }

    // A custom grouping must point every filament at a nozzle of its diameter, or the slice refuses it. With
    // the mode on, "Manual" is spelled "Nozzle Manual" and carries the per-filament nozzle map; off, it goes
    // back to "Manual" (edited in place: set_filament_map_mode() would clear the maps).
    auto *mode_opt = project.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true);
    if (m_mode) {
        std::vector<int> fmap(n_fil, 1), nmap(n_fil, 0);
        for (size_t i = 0; i < n_fil; ++i) {
            int slot = i < m_fil_diameter.size() ? match_slot(m_slots, m_ams_slot, m_fil_diameter[i], m_fil_volume[i]) : -1;
            if (slot < 0)
                slot = 0;
            nmap[i] = slot;
            fmap[i] = extruder_of_slot(m_slots, slot) + 1;
        }
        if (mode_opt->value == fmmManual)
            mode_opt->value = fmmNozzleManual;
        if (mode_opt->value == fmmNozzleManual) {
            project.option<ConfigOptionInts>("filament_map", true)->values        = fmap;
            project.option<ConfigOptionInts>("filament_nozzle_map", true)->values = nmap;
        }
        for (PartPlate *plate : plates.get_plate_list()) {
            if (!plate)
                continue;
            if (auto *pm = plate->config()->option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode"); pm && pm->value == fmmManual)
                pm->value = fmmNozzleManual;
            if (plate->get_filament_map_mode() == fmmNozzleManual) {
                plate->set_filament_maps(fmap);
                plate->set_filament_nozzle_maps(nmap);
            }
        }
    } else {
        if (mode_opt->value == fmmNozzleManual)
            mode_opt->value = fmmManual;
        for (PartPlate *plate : plates.get_plate_list())
            if (plate)
                if (auto *pm = plate->config()->option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode"); pm && pm->value == fmmNozzleManual)
                    pm->value = fmmManual;
    }

    plater->update_project_dirty_from_presets();
    pb.export_selections(*wxGetApp().app_config);
    plater->on_config_change(pb.full_config());
    plater->update();
    m_applied = true;
}

}} // namespace Slic3r::GUI
