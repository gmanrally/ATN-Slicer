#ifndef slic3r_GUI_MachineNozzleSlots_hpp_
#define slic3r_GUI_MachineNozzleSlots_hpp_

#include <string>
#include <vector>

// Turning what a connected printer reports about its tools into the slot order the project's nozzle
// inventory uses (extruder_nozzle_slot). Same placement rules as Bambu Studio 2.9.1's
// placed_nozzles() / collect_nozzle_configs_from_machine() (src/slic3r/GUI/ExtruderHelpers.cpp), kept free
// of wx and of the device classes so the rules can be tested on their own: see
// NozzleSizesDialog::read_from_printer() for the caller that feeds it from MachineObject.
namespace Slic3r { namespace GUI { namespace MachineNozzleSlots {

// One tool position as the printer reports it.
struct Reported
{
    int         extruder = 0;      // extruder index in the printer preset (0 = left, or the only one)
    bool        on_rack  = false;  // parked on the rack; false: mounted on the extruder
    int         id       = -1;     // rack position, or the nozzle id on the extruder
    int         addr_id  = -1;     // the slot the printer says this tool belongs to; -1 when it does not say
    bool        exists   = true;   // false: the printer reports the position vacant
    bool        usable   = true;   // an identified, healthy hotend (not unknown, faulty, a cutter or a pen)
    std::string diameter;          // "0.2" / "0.4" ... ; empty when the printer gave none
    int         volume   = 0;      // NozzleVolumeType
};

// What one slot of an extruder turned out to hold.
struct SlotRead
{
    enum State {
        Vacant,        // nothing reported for this slot, or the printer says it is empty
        Unidentified,  // something sits here that cannot be printed with as reported
        Nozzle         // a usable nozzle: diameter and volume are valid
    };
    State       state = Vacant;
    std::string diameter;
    int         volume = 0;
};

// The slots of one extruder, in slot order. Rack tools go to the slot the printer names for them (addr id,
// else the rack position). The tool on the extruder then goes on top: to its addr id when it has one, else to
// the first slot holding no actual tool, since that is the rack position it was taken from. A slot id the
// extruder does not have is dropped rather than clamped: an empty slot reads as "check this", a nozzle shown
// in the wrong place reads as fact.
inline std::vector<SlotRead> place(const std::vector<Reported> &tools, int extruder, int slot_count)
{
    std::vector<SlotRead> out;
    if (slot_count <= 0)
        return out;
    std::vector<const Reported *> placed(size_t(slot_count), nullptr);
    auto put = [&placed](int slot, const Reported *tool) {
        if (slot >= 0 && slot < int(placed.size()))
            placed[size_t(slot)] = tool;
    };

    for (const Reported &tool : tools)
        if (tool.extruder == extruder && tool.on_rack)
            put(tool.addr_id >= 0 ? tool.addr_id : tool.id, &tool);

    for (const Reported &tool : tools) {
        if (tool.extruder != extruder || tool.on_rack)
            continue;
        int slot = tool.addr_id;
        if (slot < 0) {
            slot = 0; // every slot already holds a tool: the mounted one wins slot 0, as Bambu Studio has it
            for (size_t i = 0; i < placed.size(); ++i)
                if (!placed[i] || !placed[i]->exists) {
                    slot = int(i);
                    break;
                }
        }
        put(slot, &tool);
    }

    out.resize(size_t(slot_count));
    for (size_t i = 0; i < placed.size(); ++i) {
        const Reported *tool = placed[i];
        if (!tool || !tool->exists)
            continue;
        if (!tool->usable || tool->diameter.empty()) {
            out[i].state = SlotRead::Unidentified;
            continue;
        }
        out[i].state    = SlotRead::Nozzle;
        out[i].diameter = tool->diameter;
        out[i].volume   = tool->volume;
    }
    return out;
}

}}} // namespace Slic3r::GUI::MachineNozzleSlots

#endif // slic3r_GUI_MachineNozzleSlots_hpp_
