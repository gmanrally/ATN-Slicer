#ifndef slic3r_FlashForge_hpp_
#define slic3r_FlashForge_hpp_

#include <vector>
#include <string>
#include <wx/string.h>
#include "PrintHost.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"
#include "../../libslic3r/PrintConfig.hpp"

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
    static bool                discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms = 10000, int idle_timeout_ms = 1500, int max_retries = 3);

private:
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
    int  get_err_code_from_body(const std::string &body) const;
    bool connect(wxString& msg) const;
    bool start_print(wxString& msg, const std::string& filename) const;
};

} // namespace Slic3r

#endif
