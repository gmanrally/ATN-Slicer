#ifndef slic3r_FlashforgeStatusPanel_hpp_
#define slic3r_FlashforgeStatusPanel_hpp_

// Native LAN status panel for Flashforge printers, shown in the Device tab in place of the embedded
// browser. The browser only ever loaded http://<printer-ip> (port 80), which the AD5M does not serve
// -- hence the ERR_CONNECTION_REFUSED users see. This polls the printer's LAN "detail" endpoint
// (port 8898, already implemented in Utils/Flashforge) and renders live status: state, progress,
// temperatures, current file. No cloud, no FlashCloud, no proprietary plugin -- pure LAN.

#include <atomic>
#include <memory>

#include <wx/panel.h>
#include <wx/timer.h>

#include "libslic3r/PrintConfig.hpp"

class wxStaticText;
class wxGauge;

namespace Slic3r {
namespace GUI {

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
    void on_timer(wxTimerEvent&);
    void poll_now();                 // spawn one worker request (no-op if one is already in flight)

    DynamicPrintConfig m_config;
    bool               m_have_printer {false};

    wxTimer            m_timer;
    // Set false in the dtor on the GUI thread; the worker's completion callback (also GUI thread via
    // CallAfter) checks it before touching this panel, so a request outliving the panel is harmless.
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::atomic<bool>  m_in_flight {false};

    wxStaticText* m_conn {nullptr};      // connection / error line
    wxStaticText* m_state {nullptr};     // printer state
    wxGauge*      m_gauge {nullptr};
    wxStaticText* m_progress {nullptr};  // "42%  |  layer 120/300  |  ~1h12m left"
    wxStaticText* m_file {nullptr};
    wxStaticText* m_nozzle {nullptr};
    wxStaticText* m_bed {nullptr};
    wxStaticText* m_chamber {nullptr};
    wxStaticText* m_footer {nullptr};    // firmware + error code
};

} // namespace GUI
} // namespace Slic3r

#endif
