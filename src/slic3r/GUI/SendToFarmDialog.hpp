#pragma once

// Orca/ATN: "Send to Farm" — push the current plate's sliced G-code (+ project
// 3MF) to the printer-farm manager. Defaults to the CLOUD farm
// (https://farm.askthenozzle.com); the URL is editable and persisted, and
// http://127.0.0.1:8000 stays selectable for the local farm. FARM_URL env
// overrides everything (dev). The cloud requires a signed-in token, sent as the
// X-Farm-Token header on every call. Sign-in is browser-based (no native password
// box): the panel opens the farm's authorize page, then polls for the token.
// Talks to the farm's /api/slicer/* endpoints:
//   POST /api/slicer/auth/start   — begin browser sign-in -> {state, authorize_url}
//   POST /api/slicer/auth/poll    — poll {state} until the browser authorises -> token
//   GET  /api/slicer/printers     — populate the printer dropdown
//   GET  /api/slicer/part-lookup  — does this part exist? next rev label?
//   POST /api/slicer/send         — create-or-get part, store files, optional queue
// On open, the part number of the opened project is pre-filled from a
// "<3mf-path>.farmctx.json" sidecar (agent-written), or the "<PN> - name.3mf"
// filename as a fallback, so a save-back lands as the NEXT revision of that part.
//
// The printer dropdown is filtered to the machine type this plate was sliced for
// (Bambu H2D g-code can't run on a Klipper K2 Plus and vice-versa): the slice's
// "kind" (bambu | klipper) is matched against each farm printer's "kind" field
// (GET /api/slicer/printers). If the farm doesn't yet return "kind", no filtering
// is applied (all printers shown), so this degrades gracefully.

#include <wx/dialog.h>
#include <string>
#include <vector>
#include <memory>

class wxTextCtrl;
class wxChoice;
class wxCheckBox;
class wxButton;
class wxStaticText;
class wxTimer;

namespace Slic3r {
namespace GUI {

class SendToFarmDialog : public wxDialog
{
public:
    explicit SendToFarmDialog(wxWindow* parent);
    ~SendToFarmDialog() override;

private:
    std::string farm_url() const;
    std::string farm_token() const;         // cloud session token (X-Farm-Token), or ""
    void fetch_printers();
    void fetch_next_part_number();          // peek + prefill the auto part number
    void apply_part_number(const std::string& pn); // use pn as the project/save name
    void lookup_part();
    void load_opened_context();             // prefill part number from sidecar/filename
    void do_sign_in();                      // start browser sign-in (opens browser, then polls)
    void poll_auth();                       // one /auth/poll tick
    void stop_auth_poll();                  // stop the poll timer + reset button
    void update_auth_ui();                  // reflect signed-in/out state
    void handle_unauthorized();             // 401 -> drop token, prompt sign-in
    void detect_slice_machine();            // fill m_slice_kind / m_slice_model from the active preset
    void do_send();

    wxTextCtrl*   m_farm_url{ nullptr };
    wxTextCtrl*   m_part_number{ nullptr };
    wxTextCtrl*   m_name{ nullptr };
    wxTextCtrl*   m_rev{ nullptr };
    wxChoice*     m_printer{ nullptr };
    wxCheckBox*   m_queue{ nullptr };
    wxStaticText* m_lookup{ nullptr };
    wxStaticText* m_compat{ nullptr };   // "Sliced for <model> — showing compatible printers"
    wxStaticText* m_auth_status{ nullptr };
    wxButton*     m_auth_btn{ nullptr };
    wxStaticText* m_status{ nullptr };
    wxButton*     m_send{ nullptr };

    std::vector<int> m_printer_ids;   // index in m_printer -> farm printer id
    bool m_have_opened_pn{ false };   // set once the opened part number is known

    // Machine this plate was sliced for — used to filter the farm printer list.
    std::string m_slice_kind;    // "bambu" | "klipper" (matches the farm's Printer.kind)
    std::string m_slice_model;   // printer_model, e.g. "Bambu Lab H2D" (display only)

    // Browser sign-in poll state.
    wxTimer*    m_auth_timer{ nullptr };
    std::string m_auth_state;
    int         m_auth_polls{ 0 };
    bool        m_auth_polling{ false };

    // ATN: shared liveness flag. Async HTTP callbacks post wxGetApp().CallAfter
    // lambdas that touch this dialog's members; if the window is closed before a
    // callback runs, the lambda would dereference freed memory. Each lambda captures
    // a copy of this flag and bails when it's false (set in the destructor).
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace GUI
} // namespace Slic3r
