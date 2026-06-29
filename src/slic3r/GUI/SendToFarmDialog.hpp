#pragma once

// Orca/ATN: "Send to Farm" — push the current plate's sliced G-code (+ project
// 3MF) to the printer-farm manager (http://127.0.0.1:8000 by default, override
// with the FARM_URL env var). Talks to the farm's /api/slicer/* endpoints:
//   GET  /api/slicer/printers     — populate the printer dropdown
//   GET  /api/slicer/part-lookup  — does this part exist? next rev label?
//   POST /api/slicer/send         — create-or-get part, store files, optional queue

#include <wx/dialog.h>
#include <string>
#include <vector>
#include <memory>

class wxTextCtrl;
class wxChoice;
class wxCheckBox;
class wxButton;
class wxStaticText;

namespace Slic3r {
namespace GUI {

class SendToFarmDialog : public wxDialog
{
public:
    explicit SendToFarmDialog(wxWindow* parent);
    ~SendToFarmDialog() override;

private:
    std::string farm_url() const;
    void fetch_printers();
    void fetch_next_part_number();          // peek + prefill the auto part number
    void apply_part_number(const std::string& pn); // use pn as the project/save name
    void lookup_part();
    void do_send();

    wxTextCtrl*   m_part_number{ nullptr };
    wxTextCtrl*   m_name{ nullptr };
    wxTextCtrl*   m_rev{ nullptr };
    wxChoice*     m_printer{ nullptr };
    wxCheckBox*   m_queue{ nullptr };
    wxStaticText* m_lookup{ nullptr };
    wxStaticText* m_status{ nullptr };
    wxButton*     m_send{ nullptr };

    std::vector<int> m_printer_ids;   // index in m_printer -> farm printer id

    // ATN: shared liveness flag. Async HTTP callbacks post wxGetApp().CallAfter
    // lambdas that touch this dialog's members; if the window is closed before a
    // callback runs, the lambda would dereference freed memory. Each lambda captures
    // a copy of this flag and bails when it's false (set in the destructor).
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace GUI
} // namespace Slic3r
