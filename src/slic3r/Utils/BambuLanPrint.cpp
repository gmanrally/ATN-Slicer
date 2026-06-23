#include "BambuLanPrint.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <regex>
#include <cstdio>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace BambuLan {

using json = nlohmann::json;

std::string sanitize_remote_name(const std::string& name)
{
    // Basename only.
    std::string base = name;
    const auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // Collapse anything outside [A-Za-z0-9._-] to '_', trim leading/trailing '_'.
    base = std::regex_replace(base, std::regex("[^A-Za-z0-9._-]+"), "_");
    const size_t b = base.find_first_not_of('_');
    const size_t e = base.find_last_not_of('_');
    base = (b == std::string::npos) ? std::string("print") : base.substr(b, e - b + 1);
    if (base.empty()) base = "print";

    // Guarantee a .3mf extension.
    auto lower_ends_with_3mf = [](const std::string& s) {
        if (s.size() < 4) return false;
        std::string tail = s.substr(s.size() - 4);
        for (auto& c : tail) c = (char) ::tolower((unsigned char) c);
        return tail == ".3mf";
    };
    if (!lower_ends_with_3mf(base)) {
        const auto dot = base.find_last_of('.');
        base = (dot == std::string::npos ? base : base.substr(0, dot)) + ".3mf";
    }
    return base;
}

namespace {
struct ProgressState { std::function<void(int)> cb; int last = -1; };

int xfer_cb(void* p, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t ultotal, curl_off_t ulnow)
{
    auto* st = reinterpret_cast<ProgressState*>(p);
    if (st && st->cb && ultotal > 0) {
        int pct = (int) ((ulnow * 100) / ultotal);
        if (pct != st->last) { st->last = pct; st->cb(pct); }
    }
    return 0; // non-zero aborts
}
} // namespace

bool ftps_upload(const std::string&       ip,
                 const std::string&       access_code,
                 const std::string&       local_path,
                 const std::string&       remote_name,
                 std::string&             err,
                 std::function<void(int)> progress_cb)
{
    if (ip.empty() || access_code.empty()) { err = "missing printer IP or access code"; return false; }

    boost::system::error_code ec;
    const auto fsize = boost::filesystem::file_size(local_path, ec);
    if (ec) { err = "cannot stat file: " + local_path; return false; }

#ifdef _WIN32
    FILE* fp = _wfopen(boost::filesystem::path(local_path).wstring().c_str(), L"rb");
#else
    FILE* fp = fopen(local_path.c_str(), "rb");
#endif
    if (!fp) { err = "cannot open file: " + local_path; return false; }

    CURL* curl = curl_easy_init();
    if (!curl) { fclose(fp); err = "curl init failed"; return false; }

    // Implicit FTPS (TLS from connect) on Bambu's port 990, self-signed cert.
    const std::string url = "ftps://" + ip + ":990/" + remote_name;
    ProgressState st; st.cb = std::move(progress_cb);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, "bblp");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long) CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // Bambu's PASV reply advertises an unroutable internal IP — reuse the control IP.
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) fsize);
    // Bounded: connect within 30s, and abort if the transfer stalls (<1 B/s) for 120s.
    // This is what guarantees we can NEVER hang the way the plugin eMMC tunnel does.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, st.cb ? 0L : 1L);
    if (st.cb) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
    }

    const CURLcode res = curl_easy_perform(curl);
    long resp = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        err = std::string("FTPS upload failed: ") + curl_easy_strerror(res)
            + " (ftp " + std::to_string(resp) + ")";
        BOOST_LOG_TRIVIAL(error) << "BambuLan::ftps_upload " << url << " -> " << err;
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "BambuLan::ftps_upload ok: " << remote_name << " (" << fsize << " bytes)";
    return true;
}

std::string build_project_file_command(const std::string&      remote_name,
                                       int                     plate,
                                       const std::string&      bed_type,
                                       bool                    use_ams,
                                       const std::vector<int>& ams_mapping,
                                       const std::string&      ams_mapping2_json)
{
    // subtask_name = file name without the .gcode.3mf / .3mf extension (cosmetic, as Bambu does).
    std::string subtask = remote_name;
    for (const char* ext : {".gcode.3mf", ".3mf"}) {
        auto pos = subtask.rfind(ext);
        if (pos != std::string::npos && pos == subtask.size() - std::char_traits<char>::length(ext)) {
            subtask = subtask.substr(0, pos); break;
        }
    }

    json pr;
    pr["command"]                  = "project_file";
    pr["param"]                    = "Metadata/plate_" + std::to_string(plate <= 0 ? 1 : plate) + ".gcode";
    pr["file"]                     = remote_name;
    pr["url"]                      = "ftp:///" + remote_name; // we uploaded it to the FTP root
    pr["subtask_name"]             = subtask;
    pr["bed_type"]                 = bed_type.empty() ? std::string("textured_plate") : bed_type;
    pr["use_ams"]                  = use_ams;
    // v0 tray map ([-1,-1] for external spools); v1 NOZZLE map ([{ams_id,slot_id},...]) is what
    // actually routes each filament to the correct H2D nozzle -- pass the slicer's value verbatim.
    pr["ams_mapping"]              = ams_mapping.empty() ? std::vector<int>{-1} : ams_mapping;
    if (!ams_mapping2_json.empty()) {
        try { json m2 = json::parse(ams_mapping2_json); if (m2.is_array()) pr["ams_mapping2"] = m2; }
        catch (...) {}
    }
    // Calibration / options -- match Bambu Studio's H2D project_file defaults.
    pr["bed_leveling"]             = false;
    pr["auto_bed_leveling"]        = 2;
    pr["flow_cali"]                = false;
    pr["vibration_cali"]           = true;
    pr["layer_inspect"]            = false;
    pr["timelapse"]                = false;
    pr["nozzle_offset_cali"]       = 2;
    pr["extrude_cali_flag"]        = 0;
    pr["extrude_cali_manual_mode"] = 0;
    pr["profile_id"]               = "0";
    pr["project_id"]               = "0";
    pr["task_id"]                  = "0";
    pr["subtask_id"]               = "0";
    pr["cfg"]                      = "0";
    pr["sequence_id"]              = "20000001";

    json p; p["print"] = pr;
    return p.dump();
}

}} // namespace Slic3r::BambuLan
