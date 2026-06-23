#ifndef slic3r_BambuLanPrint_hpp_
#define slic3r_BambuLanPrint_hpp_

// ATN: Native "send to Bambu over LAN" path. The closed Bambu networking plugin that
// OrcaSlicer ships cannot push a print to newer printers (e.g. H2D) — its eMMC tunnel
// hangs and the FTP fallback is rejected by firmware (error 0500-4002). Bambu Studio
// works because it uses Bambu's *current* closed plugin, which OrcaSlicer can't load.
//
// We don't need the whole plugin: discovery + status (live temps, pushed state) already
// work through the existing plugin's MQTT link. Only the *send* is broken. So we do just
// the file upload ourselves over implicit FTPS (the open, documented Bambu LAN protocol),
// then publish the normal `project_file` print command over the existing MQTT connection
// via MachineObject::publish_json(). Recipe ported verbatim from the proven printer-farm
// implementation (app/bambu.py, which drives the H2D in production).

#include <string>
#include <vector>
#include <functional>

namespace Slic3r { namespace BambuLan {

// Make a filename safe for FTP STOR + the `ftp:///<name>` print url (Bambu chokes on
// spaces/odd chars in the url); guarantees a .3mf extension.
std::string sanitize_remote_name(const std::string& name);

// Upload a local .3mf to the printer's FTP root via implicit FTPS (port 990, user "bblp",
// password = LAN access code, self-signed cert). Returns true on success. Has a bounded
// stall timeout so it can never hang indefinitely the way the plugin tunnel does.
// progress_cb(percent 0..100) is optional.
bool ftps_upload(const std::string&            ip,
                 const std::string&            access_code,
                 const std::string&            local_path,
                 const std::string&            remote_name,
                 std::string&                  err,
                 std::function<void(int)>      progress_cb = nullptr);

// Build the `project_file` MQTT command (as a JSON string) for a file already uploaded to
// the printer's FTP root. bed_type MUST match the plate the .3mf was sliced for (e.g.
// "textured_plate" / "eng_plate") or the printer HMS-rejects at prepare.
//
// ams_mapping is the v0 tray array (e.g. [-1,-1] for external spools). ams_mapping2_json is
// the slicer's task_ams_mapping2 string -- the per-filament NOZZLE selector that the H2D needs
// ([{"ams_id":254,"slot_id":0},{"ams_id":255,"slot_id":0}] = left/right external). Pass it
// through verbatim; empty for single-nozzle jobs. Matches what Bambu Studio publishes.
std::string build_project_file_command(const std::string&        remote_name,
                                        int                       plate,
                                        const std::string&        bed_type,
                                        bool                      use_ams,
                                        const std::vector<int>&   ams_mapping,
                                        const std::string&        ams_mapping2_json);

}} // namespace Slic3r::BambuLan

#endif // slic3r_BambuLanPrint_hpp_
