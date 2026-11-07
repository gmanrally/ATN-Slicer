#ifndef slic3r_FlashForge_hpp_
#define slic3r_FlashForge_hpp_

#include <vector>
#include <string>
#include <wx/string.h>
#include "PrintHost.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"

namespace Slic3r { enum GCodeFlavor : unsigned char; }

namespace Slic3r {
class DynamicPrintConfig;
class Http;

struct FlashforgeMaterialSlot
{
    int         slot_id {0}; // API is 1-based.
    bool        has_filament {false};
    std::string material_name;
    std::string material_color;
};

struct FlashforgeDiscoveredPrinter
{
    std::string name;
    std::string serial_number;
    std::string ip_address;
};

// Live status from the printer's LAN "detail" endpoint (port 8898). Populated by fetch_status().
// This is the data the native Device panel shows in place of the embedded browser, which only ever
// pointed at http://<ip> (port 80) -- a page the AD5M does not serve. `valid` distinguishes a genuine
// reading from a default-constructed blank; temperatures/counts are <0 when the printer omitted them.
struct FlashforgeStatus
{
    bool        valid {false};
    std::string state;                 // raw "status" string (printing / ready / pause / completed / busy ...)
    std::string file_name;             // printFileName
    std::string firmware;              // firmwareVersion
    std::string error_code;            // errorCode ("0" / "" when healthy)
    std::string camera_url;            // cameraStreamUrl (empty if none)
    double      progress {-1.0};       // printProgress, 0..1 (-1 = unknown)
    int         layer {-1};            // printLayer
    int         total_layers {-1};     // targetPrintLayer
    double      nozzle_temp {-1.0};    // rightTemp
    double      nozzle_target {-1.0};  // rightTargetTemp
    double      bed_temp {-1.0};       // platTemp
    double      bed_target {-1.0};     // platTargetTemp
    double      chamber_temp {-1.0};   // chamberTemp
    double      est_time_s {-1.0};     // estimatedTime (seconds)

    // Live-control readbacks (newer firmware only; best-effort -- a sentinel means the printer did not
    // report that field, and the Device panel falls back to the last value the user set instead).
    int         speed_pct {-1};        // current feed-rate override %, if reported
    double      z_comp {-1000.0};      // live Z compensation (mm); -1000 = unknown
    int         light_on {-1};         // chamber light: 1 on / 0 off / -1 unknown
    int         internal_fan {-1};     // internal circulation: 1 on / 0 off / -1 unknown
    int         external_fan {-1};     // external circulation: 1 on / 0 off / -1 unknown
    std::string material;              // right-extruder filament type, if reported
};

class Flashforge : public PrintHost
{
public:
    explicit Flashforge(DynamicPrintConfig *config);
    ~Flashforge() override = default;

    const char *get_name() const override;

    bool                       test(wxString &curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString &msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return true; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }
    bool                       fetch_material_slots(std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station, wxString& msg) const;
    // Poll the LAN detail endpoint into a FlashforgeStatus. Blocking (perform_sync) -- call off the
    // GUI thread. Returns false + fills msg on any transport/JSON/auth failure; out.valid is the
    // authoritative success flag for callers that ignore the return.
    bool                       fetch_status(FlashforgeStatus& out, wxString& msg) const;
    // Parses a local API "detail" reply. Returns false when the body is not valid JSON.
    static bool                parse_material_slots(const std::string& body, std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station);
    static bool                discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms = 10000, int idle_timeout_ms = 1500, int max_retries = 3);

    // --- LAN control (port 8898, POST /control) --------------------------------------------------
    // Write commands the native Device panel drives. Each is blocking (perform_sync) -- call OFF the
    // GUI thread. Returns false + fills msg on any transport/auth/err failure. The command shapes
    // follow the documented FlashForge local API (Parallel-7/flashforge-api-docs); they are NOT
    // verified against on-bench hardware here (we have none), so treat as best-effort until the first
    // run against a real Adventurer 5M / AD5X.
    static constexpr double    TEMP_LEAVE = -200.0;   // sentinel: "leave this temperature channel unchanged"

    bool                       job_control(const std::string& action, wxString& msg) const;                     // "pause" | "continue" | "cancel"
    bool                       set_temperatures(double nozzle_c, double bed_c, double chamber_c, wxString& msg) const; // TEMP_LEAVE keeps a channel
    bool                       set_speed(int percent, wxString& msg) const;                                      // feed-rate override %
    bool                       set_z_compensation(double mm, wxString& msg) const;                               // live Z offset, -5..+5 mm
    bool                       set_light(bool on, wxString& msg) const;                                          // chamber LED
    bool                       set_circulation(int internal_on, int external_on, wxString& msg) const;           // 1 on / 0 off / <0 leave (5M Pro)

private:
    bool                       send_control(const std::string& cmd, const std::string& args_json, wxString& msg) const;
    std::string m_host;
    std::string m_serial_number;
    std::string m_check_code;
    std::string m_console_port;
    const int m_bufferSize;
    GCodeFlavor m_gcFlavor;
    Slic3r::Utils::SerialMessage controlCommand          = {"~M601 S1\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectKlipperCommand   = {"~M640\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectLegacyCommand    = {"~M650\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage nozzlePosCommand        = {"~M114\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage deviceInfoCommand       = {"~M115\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage statusCommand           = {"~M119\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage tempStatusCommand       = {"~M105\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage printStatusCommand      = {"~M27\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage saveFileCommand         = {"~M29\r\n",Slic3r::Utils::Command};
    bool upload_local_api(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn) const;
    bool test_local_api(wxString& msg) const;
    bool request_local_api_json(const std::string& path, const std::string& body, std::string& response_body, wxString& error_msg) const;
    std::string make_http_url(const std::string& path) const;
    std::string extract_host_name() const;
    bool connect(wxString& msg) const;
    bool start_print(wxString& msg, const std::string& filename) const;
};

} // namespace Slic3r

#endif
