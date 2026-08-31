#pragma once

// ATN: "Report a problem" — a native Help-menu dialog that emails a problem report
// (with a recent diagnostic log attached) to the operator, via the cloud endpoint
//   POST <atn base>/api/slicer/report-problem
// It's always available, even when the assistant panel is closed or the cloud AI is
// down. The sandboxed assistant panel has its own lighter web form (no log attach).

#include <wx/dialog.h>
#include <string>
#include <memory>

class wxTextCtrl;
class wxCheckBox;
class wxStaticText;
class Button;   // Orca themed button (Widgets/Button.hpp)

namespace Slic3r {
namespace GUI {

class ReportProblemDialog : public wxDialog
{
public:
    explicit ReportProblemDialog(wxWindow* parent);
    ~ReportProblemDialog() override;

private:
    void do_send();

    wxTextCtrl*   m_message{ nullptr };
    wxTextCtrl*   m_email{ nullptr };
    wxCheckBox*   m_include_log{ nullptr };
    wxStaticText* m_status{ nullptr };
    Button*       m_send{ nullptr };

    // Async HTTP callbacks post CallAfter lambdas that touch this dialog; each copies
    // this flag and bails if the dialog was closed first (set false in the destructor).
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace GUI
} // namespace Slic3r
