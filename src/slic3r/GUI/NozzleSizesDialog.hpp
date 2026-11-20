#ifndef slic3r_GUI_NozzleSizesDialog_hpp_
#define slic3r_GUI_NozzleSizesDialog_hpp_

#include <string>
#include <vector>

#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

class wxBoxSizer;
class wxFlexGridSizer;
class wxPanel;
class wxWindow;
class Button;
class CheckBox;
class ComboBox;
class Label;

namespace Slic3r { namespace GUI {

// Mixed nozzle sizes in one print (Bambu Studio 2.9's multi_nozzle_diameter_mode; the engine side came in
// ATN Slicer 2.7.0, this is the in-app selector it promised). The dialog records which nozzle sits in each
// slot of the printer (diameter + flow type, or an empty slot) and which of those each filament prints
// with, and writes the project options the engine already reads: multi_nozzle_diameter_mode, the per-slot
// inventory (extruder_nozzle_count / _diameter / _volume_type / _slot), filament_diameter_map and
// filament_volume_map. A plate in a custom grouping also gets its per-filament nozzle map from the same
// picks (filament_nozzle_map / filament_map, mode "Nozzle Manual"), so it can never point a filament at a
// nozzle of another diameter. Orca-native replacement for Bambu Studio's per-row FilamentNozzlePopup.
// "Read from printer" fills the slots from the nozzle report of the connected printer.
class NozzleSizesDialog : public DPIDialog
{
public:
    explicit NozzleSizesDialog(wxWindow *parent);

    // True once Apply wrote the project (false after Cancel).
    bool applied() const { return m_applied; }

    struct Slot
    {
        std::string      diameter;             // "0.2", "0.4" … (format_diameter_to_str spelling)
        NozzleVolumeType volume{nvtStandard};
        bool             installed{true};      // false: the physical slot carries no nozzle
    };

    // Which physical slot (flat index across extruders, in slot order) a filament of this diameter and flow
    // prints on: the AMS-fed slot when it fits, else the first installed exact match, else the first slot
    // of that diameter whatever its flow; -1 when no installed slot has the diameter.
    static int match_slot(const std::vector<std::vector<Slot>> &slots, int ams_slot, const std::string &diameter,
                          NozzleVolumeType volume);
    // The extruder a flat slot index belongs to (0 when out of range).
    static int extruder_of_slot(const std::vector<std::vector<Slot>> &slots, int flat_slot);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct SlotRow { ComboBox *diameter = nullptr; ComboBox *flow = nullptr; };
    struct FilRow  { ComboBox *diameter = nullptr; ComboBox *flow = nullptr; };

    void read_state();
    void build();
    void fill_slot_diameter(size_t e, size_t k, SlotRow &row);
    void fill_slot_flow(size_t e, size_t k, SlotRow &row);
    void fill_filament_flow(size_t i, FilRow &row);
    void on_slot_diameter(size_t e, size_t k);
    void on_slot_flow(size_t e, size_t k);
    void on_mode_toggled();
    void rebuild_filament_rows();
    void read_from_printer();
    void set_read_status(const wxString &text, bool warning);

    std::vector<std::string>      installed_diameters() const;
    std::vector<NozzleVolumeType> installed_volumes_for(const std::string &diameter) const;
    const Slot                   *slot_at(int flat_slot) const;
    wxString                      volume_label(NozzleVolumeType v) const;
    bool                          validate(wxString &why) const;
    void                          apply();

    // --- state ---
    bool                                       m_mode = false;
    bool                                       m_single_ext_multi_nozzle = false;
    std::vector<std::string>                   m_printer_diameters;         // every diameter the printer is sold in
    std::vector<std::string>                   m_extruder_default_diameter; // the selected machine variant, per extruder
    std::vector<std::vector<Slot>>             m_slots;                     // per extruder, per slot
    std::vector<std::vector<NozzleVolumeType>> m_offered;                   // per extruder: flow types its variants offer
    int                                        m_ams_slot = -1;             // flat slot the AMS feeds, -1 none / not a rack
    std::vector<std::string>                   m_fil_name;
    std::vector<std::string>                   m_fil_colour;
    std::vector<std::string>                   m_fil_diameter;
    std::vector<NozzleVolumeType>              m_fil_volume;
    bool                                       m_applied = false;

    // --- widgets ---
    CheckBox                          *m_mode_check = nullptr;
    Label                             *m_hint       = nullptr;
    Button                            *m_read_btn    = nullptr;
    Label                             *m_read_status = nullptr;
    std::vector<std::vector<SlotRow>>  m_slot_rows;
    wxPanel                           *m_fil_panel  = nullptr;
    wxFlexGridSizer                   *m_fil_sizer  = nullptr;
    std::vector<FilRow>                m_fil_rows;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_NozzleSizesDialog_hpp_
