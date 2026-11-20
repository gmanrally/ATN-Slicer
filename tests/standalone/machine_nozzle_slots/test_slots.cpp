// Standalone checks for MachineNozzleSlots::place. Built against the header found on the include path
// (-I), so the same file runs against deliberately broken copies to prove each rule is pinned.
#include "MachineNozzleSlots.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace Slic3r::GUI::MachineNozzleSlots;

static int g_fail = 0, g_run = 0;
static std::string show(const std::vector<SlotRead> &s)
{
    std::string o;
    for (const auto &r : s) {
        if (!o.empty()) o += " ";
        if (r.state == SlotRead::Vacant) o += "-";
        else if (r.state == SlotRead::Unidentified) o += "?";
        else o += r.diameter + "/" + std::to_string(r.volume);
    }
    return o;
}
static void check(const char *name, const std::vector<SlotRead> &got, const std::string &want)
{
    ++g_run;
    const std::string g = show(got);
    if (g != want) { ++g_fail; std::printf("FAIL %s\n  got : %s\n  want: %s\n", name, g.c_str(), want.c_str()); }
    else std::printf("ok   %s\n", name);
}
static Reported rack(int ext, int id, const char *d, int vol = 0, int addr = -1)
{
    Reported r; r.extruder = ext; r.on_rack = true; r.id = id; r.addr_id = addr; r.diameter = d; r.volume = vol; return r;
}
static Reported mounted(int ext, int id, const char *d, int vol = 0, int addr = -1)
{
    Reported r; r.extruder = ext; r.on_rack = false; r.id = id; r.addr_id = addr; r.diameter = d; r.volume = vol; return r;
}

int main()
{
    // H2D: one nozzle per extruder, left = extruder 0 (device nozzle id 1), right = extruder 1 (id 0).
    {
        std::vector<Reported> t = {mounted(1, 0, "0.4", 1), mounted(0, 1, "0.2", 0)};
        check("h2d left", place(t, 0, 1), "0.2/0");
        check("h2d right", place(t, 1, 1), "0.4/1");
    }
    // H2C right extruder: six slots, rack positions 0,1,2,4,5 parked, position 3 vacated by the mounted one.
    {
        std::vector<Reported> t = {rack(1, 0, "0.4"), rack(1, 1, "0.4"), rack(1, 2, "0.2"), rack(1, 4, "0.6", 1),
                                   rack(1, 5, "0.4"), mounted(1, 0, "0.8"), mounted(0, 1, "0.4")};
        check("h2c mounted fills the vacated rack slot", place(t, 1, 6), "0.4/0 0.4/0 0.2/0 0.8/0 0.6/1 0.4/0");
        check("h2c left untouched by the rack", place(t, 0, 1), "0.4/0");
    }
    // The printer reports the vacated position as a placeholder that does not exist: still the free slot.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4"), rack(0, 1, ""), rack(0, 2, "0.4"), mounted(0, 0, "0.6")};
        t[1].exists = false;
        check("placeholder slot counts as free", place(t, 0, 3), "0.4/0 0.6/0 0.4/0");
    }
    // H3C: nine slots, every tool carries the slot it belongs to; the mounted tool keeps ITS slot (7), not
    // the first free one (2), and the report order does not matter.
    {
        std::vector<Reported> t = {mounted(0, 0, "0.2", 0, 7), rack(0, 0, "0.4", 0, 0), rack(0, 1, "0.4", 1, 1),
                                   rack(0, 3, "0.6", 0, 3), rack(0, 8, "0.4", 4, 8)};
        check("h3c addr ids", place(t, 0, 9), "0.4/0 0.4/1 - 0.6/0 - - - 0.2/0 0.4/4");
    }
    // addr id wins over the rack position when they disagree.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4", 0, 5)};
        check("addr id beats rack position", place(t, 0, 6), "- - - - - 0.4/0");
    }
    // A cutter / pen / unknown hotend holds its slot but is not a nozzle; no diameter is the same.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4"), rack(0, 1, "0.4"), rack(0, 2, "")};
        t[1].usable = false;
        check("unusable tool and missing diameter", place(t, 0, 4), "0.4/0 ? ? -");
    }
    // A slot id the extruder does not have is dropped, never clamped onto the last slot.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4"), rack(0, 9, "0.8"), rack(0, -3, "0.8")};
        check("out of range dropped", place(t, 0, 3), "0.4/0 - -");
    }
    // Rack full and one more on the extruder: Bambu Studio lets the mounted tool take slot 0.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4"), rack(0, 1, "0.4"), mounted(0, 0, "0.6")};
        check("rack full, mounted takes slot 0", place(t, 0, 2), "0.6/0 0.4/0");
    }
    // The mounted tool is placed AFTER the rack whatever the report order: it must not be overwritten.
    {
        std::vector<Reported> t = {mounted(0, 0, "0.6"), rack(0, 0, "0.4"), rack(0, 1, "0.4")};
        check("mounted listed first still placed last", place(t, 0, 3), "0.4/0 0.4/0 0.6/0");
    }
    // A position the printer reports as absent is vacant even when it still carries the last nozzle's data.
    {
        std::vector<Reported> t = {rack(0, 0, "0.4"), rack(0, 1, "0.8", 1), rack(0, 2, "0.4")};
        t[1].exists = false;
        check("absent tool with stale data is vacant", place(t, 0, 3), "0.4/0 - 0.4/0");
    }
    check("no slots", place({}, 0, 0), "");
    check("nothing reported", place({}, 0, 3), "- - -");

    std::printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
