#pragma once

// ATN: the two decisions behind the assistant's one-click Apply (AtnPanel::handle_set_setting),
// kept free of slicer and wx types so they can be checked on their own:
//
//     g++ -std=c++17 -I src tests/atn/atn_apply_rules_test.cpp -o atn_apply_rules_test
//
// Both come from one customer evening (2026-11-20):
//  - "Force outer brim" was sent as brim_type = outer_brim. That is not a choice (outer_only is),
//    and the slicer's forward-compatibility rule quietly swapped it for the setting's DEFAULT, so
//    Apply set Auto and said "already auto_brim".
//  - Part cooling fan minimum was sent as "80,60", one figure per filament on the plate. The whole
//    list was loaded into whichever filament was open in the Filament tab, which then used the
//    first figure: filament 2 got filament 1's value.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace AtnApply {

// Lower case, with spaces, hyphens and underscores dropped: "Outer brim only" reads "outerbrimonly".
inline std::string fold(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
        if (c != ' ' && c != '_' && c != '-')
            out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// The stored value of a choice setting that `value` means, or "" when it means none of them or
// could mean more than one. `values` are the stored values and `labels` their English labels,
// index for index (labels may be shorter).
//
// A stored value is taken as it is. Otherwise the choices are compared folded, first whole
// ("Outer brim only" is brim_type outer_only), then, for three letters or more, as a run of
// letters inside a stored value or label: "outer_brim" is in "Outer brim only" and nowhere else,
// so it is outer_only, and "top surfaces" is in "All top surfaces" alone, so it is ironing_type
// top; "tree" is in both tree(auto) and tree(manual), so it is nothing.
inline std::string resolve_choice(const std::vector<std::string>& values, const std::vector<std::string>& labels,
                                  const std::string& value)
{
    for (const std::string& v : values)
        if (v == value)
            return v;

    const std::string want = fold(value);
    if (want.empty())
        return {};

    for (int pass = 0; pass < 2; ++pass) {
        const bool   by_part = pass == 1;
        if (by_part && want.size() < 3)
            break;
        std::size_t  found = values.size();
        bool         clash = false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const std::string as_value = fold(values[i]);
            const std::string as_label = i < labels.size() ? fold(labels[i]) : std::string();
            const bool hit = by_part ? (as_value.find(want) != std::string::npos || as_label.find(want) != std::string::npos)
                                     : (as_value == want || (!as_label.empty() && as_label == want));
            if (!hit)
                continue;
            if (found != values.size() && values[found] != values[i])
                clash = true;
            found = i;
        }
        if (clash)
            return {};
        if (found != values.size())
            return values[found];
    }
    return {};
}

// Which figure of a list the assistant sent belongs to the filament open in the Filament tab.
inline constexpr std::size_t kWholeList = static_cast<std::size_t>(-1);   // the list is this filament's own, use it as sent
inline constexpr std::size_t kNoFit     = static_cast<std::size_t>(-2);   // cannot tell: refuse rather than guess
inline constexpr std::size_t kNotOnPlate = static_cast<std::size_t>(-3);  // `slot` when the open filament is on no plate slot

//  incoming  how many figures were sent
//  slots     how many filaments the plate has (the assistant sees one figure per slot)
//  own       how many figures the open filament's preset keeps for the setting: one, or one per
//            nozzle variant on a multi-variant filament
//  slot      the open filament's position on the plate (0-based), or kNotOnPlate
//  all_same  every figure sent is the same
//
// Returns the index of the figure to use, kWholeList, or kNoFit.
inline std::size_t figure_for_open_filament(std::size_t incoming, std::size_t slots, std::size_t own, std::size_t slot,
                                            bool all_same)
{
    if (incoming == 0)
        return kNoFit;
    if (incoming == 1)
        return 0;                       // one figure: it is for the filament being edited
    if (incoming == slots) {
        if (slot < slots)
            return slot;                // one per plate filament: this filament's own
        return all_same ? 0 : kNoFit;   // the open filament is not on the plate: only safe if they agree
    }
    if (incoming == own)
        return kWholeList;              // one per nozzle variant of this filament
    return all_same ? 0 : kNoFit;
}

} // namespace AtnApply
} // namespace GUI
} // namespace Slic3r
