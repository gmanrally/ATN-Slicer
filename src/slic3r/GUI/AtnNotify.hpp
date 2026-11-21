#pragma once

// ATN: notifications from the code that sends a job to the docked assistant panel.
//
// Kept apart from AtnPanel.hpp and Plater.hpp on purpose: the send paths (the print host upload
// queue, the Bambu job handlers) have no handle on the panel and need nothing else from it, and a
// declaration added to either of those headers recompiles most of the GUI.

#include <string>

namespace Slic3r {
namespace GUI {

// A job has reached a printer directly, without the farm: `route` is "host" for an upload through
// a print host (Klipper / Moonraker, OctoPrint, PrusaLink, FlashForge, Creality, Elegoo, Qidi ...)
// and "bambu" for a Bambu print or send. The page records the job in the learning diary, exactly
// as it does a farm dispatch (AtnPanel::on_sent_to_farm), so its outcome can be logged later.
//
// Call it on the GUI thread, once the job is known to have arrived. Does nothing when the
// assistant panel does not exist.
void atn_notify_sent_to_printer(const std::string& route);

} // namespace GUI
} // namespace Slic3r
