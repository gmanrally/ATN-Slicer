#ifndef slic3r_QidiPrinterLink_hpp_
#define slic3r_QidiPrinterLink_hpp_

#include <string>
#include <wx/string.h>

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// ATN: live state of a Qidi (Klipper + Moonraker) printer, read for the "Send print job" dialog.
// Everything here is printer-side state: the four job options Qidi Studio shows on its send dialog
// are NOT written into the G-code, they are variables the printer's own start macros consult
// (see QidiPrinterLink::apply for the exact mechanism of each).
struct QidiPrinterState
{
    bool        online{false};                 // Moonraker answered /printer/info
    std::string klippy_state;                  // ready / startup / shutdown / error / disconnected
    std::string print_state;                   // print_stats.state: standby / printing / paused / complete / cancelled / error
    std::string hostname;                      // /printer/info hostname
    int         box_count{0};                  // save_variables.box_count (0 = no QIDI Box fitted)
    bool        has_box_switch{false};         // save_variables.enable_box exists
    bool        enable_box{false};             // save_variables.enable_box
    bool        has_polar_cooler{false};       // save_variables.enable_polar_cooler exists
    bool        polar_cooler{true};            // save_variables.enable_polar_cooler (the M1004 macro defaults it to on)
    bool        has_timelapse{false};          // Moonraker timelapse component answered
    bool        timelapse{false};              // its `enabled`
    bool        has_bed_leveling{false};       // gcode_macro _km_globals.bedmesh_before_print exists
    bool        bed_leveling{true};            // its value: fresh adaptive mesh before the print, else load the saved mesh
    bool        filament_detected{false};      // filament_switch_sensor
    std::string external_filament;             // save_variables.filament_slot16 as text ("" when unset)
};

// What the user chose on the dialog. `write_*` says whether the printer supports (and the dialog
// offered) that option; unsupported ones are left untouched on the printer.
struct QidiSendOptions
{
    bool timelapse{false};
    bool bed_leveling{true};
    bool enable_box{false};
    bool polar_cooler{true};
    bool write_timelapse{false};
    bool write_bed_leveling{false};
    bool write_enable_box{false};
    bool write_polar_cooler{false};
};

class QidiPrinterLink
{
public:
    // Reads print_host / printhost_apikey from the printer preset (physical printer) config.
    explicit QidiPrinterLink(const DynamicPrintConfig& config);

    // Fills `state` from the printer. Returns false (with `error`) when the printer cannot be reached;
    // `state.online` mirrors that. Partial information is still filled in on a partial failure.
    bool query(QidiPrinterState& state, wxString& error) const;

    // Pushes the chosen options to the printer, each through the mechanism its firmware actually
    // reads. Stops at the first failure and reports which option failed.
    bool apply(const QidiSendOptions& options, wxString& error) const;

    const std::string& host() const { return m_host; }

private:
    std::string make_url(const std::string& path) const;
    void        set_auth(Http& http) const;
    bool        get_json(const std::string& path, std::string& body, wxString& error) const;
    bool        post_json(const std::string& path, const std::string& body, wxString& error) const;
    bool        run_gcode(const std::string& script, wxString& error) const;

    std::string m_host;
    std::string m_apikey;
};

} // namespace Slic3r

#endif
