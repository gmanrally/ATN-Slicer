#ifndef slic3r_FlashforgeStatusPanel_hpp_
#define slic3r_FlashforgeStatusPanel_hpp_

// Native LAN status panel for Flashforge printers, shown in the Device tab in place of the embedded
// browser. The browser only ever loaded http://<printer-ip> (port 80), which the AD5M does not serve
// -- hence the ERR_CONNECTION_REFUSED users see. This polls the printer's LAN "detail" endpoint
// (port 8898, already implemented in Utils/Flashforge) and renders live status: state, progress,
// temperatures, current file. No cloud, no FlashCloud, no proprietary plugin -- pure LAN.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

#include "libslic3r/PrintConfig.hpp"

class wxStaticText;
class wxGauge;
class wxButton;
class wxSpinCtrl;
class wxCheckBox;

namespace Slic3r {

class Flashforge;
struct FlashforgeStatus;

namespace GUI {

class FlashforgeCameraView;

class FlashforgeStatusPanel : public wxPanel
{
public:
    explicit FlashforgeStatusPanel(wxWindow* parent);
    ~FlashforgeStatusPanel() override;

    // Point the panel at a printer and (re)start polling. Stores a copy of the printer config so the
    // worker thread never touches shared GUI state. Safe to call on printer change.
    void set_printer(const DynamicPrintConfig& config);

    // Polling follows visibility: no point hammering the printer while the tab is hidden.
    bool Show(bool show = true) override;

private:
    void build_ui();
    void on_expand_camera();         // open the live view in a large, resizable pop-out window
    void on_timer(wxTimerEvent&);
    void poll_now();                 // spawn one worker request (no-op if one is already in flight)

    // Fire a LAN control command on a worker thread, then refresh. `op` runs off the GUI thread and
    // must only touch the Flashforge client it is handed. `busy_hint` is shown while it runs.
    void run_control(std::function<bool(const Flashforge&, wxString&)> op, const wxString& busy_hint);
    void update_controls(const FlashforgeStatus& st);  // sync control labels/enable-state from a reading
    void set_controls_online(bool online);             // grey the whole control set out when offline

    DynamicPrintConfig m_config;
    bool               m_have_printer {false};

    wxTimer            m_timer;
    // Set false in the dtor on the GUI thread; the worker's completion callback (also GUI thread via
    // CallAfter) checks it before touching this panel, so a request outliving the panel is harmless.
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::atomic<bool>  m_in_flight {false};

    FlashforgeCameraView* m_camera {nullptr};   // live MJPEG view, shown only when the printer reports a stream URL
    wxButton*             m_btn_expand {nullptr}; // opens the camera in a large, resizable pop-out window
    wxString              m_camera_url;           // last stream URL, so the pop-out streams the same feed
    bool                  m_camera_expanded {false}; // pop-out open: hand it the single stream; poll leaves the docked feed alone

    wxStaticText* m_conn {nullptr};      // connection / error line
    wxStaticText* m_state {nullptr};     // printer state
    wxGauge*      m_gauge {nullptr};
    wxStaticText* m_progress {nullptr};  // "42%  |  layer 120/300  |  ~1h12m left"
    wxStaticText* m_file {nullptr};
    wxStaticText* m_nozzle_cur {nullptr};
    wxStaticText* m_nozzle_tgt {nullptr};
    wxStaticText* m_bed_cur {nullptr};
    wxStaticText* m_bed_tgt {nullptr};
    wxStaticText* m_chamber_cur {nullptr};
    wxStaticText* m_chamber_tgt {nullptr};
    wxStaticText* m_footer {nullptr};    // firmware + error code

    // --- controls (extend the read-only panel to parity with FlashForge's own Device page) --------
    wxButton*     m_btn_pauseresume {nullptr};   // label flips Pause/Resume with state
    wxButton*     m_btn_cancel {nullptr};
    wxSpinCtrl*   m_set_nozzle {nullptr};        // target editors
    wxSpinCtrl*   m_set_bed {nullptr};
    wxButton*     m_btn_set_nozzle {nullptr};
    wxButton*     m_btn_set_bed {nullptr};
    wxStaticText* m_speed_val {nullptr};         // live feed-rate override %
    wxStaticText* m_z_val {nullptr};             // live Z compensation
    wxStaticText* m_extra {nullptr};             // material / speed line on the job card
    wxButton*     m_btn_light {nullptr};         // chamber light toggle
    wxCheckBox*   m_int_circ {nullptr};          // internal circulation (5M Pro)
    wxCheckBox*   m_ext_circ {nullptr};          // external circulation (5M Pro)

    // Last-known live values, so the +/- steppers act optimistically between polls.
    int           m_cur_speed {100};
    double        m_cur_z {0.0};
    int           m_light_state {-1};            // 1 on / 0 off / -1 unknown
    std::string   m_state_raw;                   // last raw state string (gates job controls)

    // Every interactive control, so the whole set can be greyed out in one call when offline.
    std::vector<wxWindow*> m_controls;
};

} // namespace GUI
} // namespace Slic3r

#endif
