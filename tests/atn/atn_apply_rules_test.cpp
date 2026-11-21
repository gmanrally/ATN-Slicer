// Checks for src/slic3r/GUI/AtnApplyRules.hpp. Standalone on purpose (no slicer, no test framework):
//
//     g++ -std=c++17 -I src tests/atn/atn_apply_rules_test.cpp -o atn_apply_rules_test && ./atn_apply_rules_test
//
// The choice lists below are copied from PrintConfig.cpp as it stood on 2026-11-21.
#include "slic3r/GUI/AtnApplyRules.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace Slic3r::GUI::AtnApply;

static int failed = 0;

static void check(const char* label, bool ok, const std::string& detail = std::string())
{
    if (ok) {
        std::printf("  ok    %s\n", label);
    } else {
        ++failed;
        std::printf("  FAIL  %s%s%s%s\n", label, detail.empty() ? "" : "  [", detail.c_str(), detail.empty() ? "" : "]");
    }
}

int main()
{
    const std::vector<std::string> brim_values = { "auto_brim", "brim_ears", "painted", "outer_only", "inner_only",
                                                   "outer_and_inner", "no_brim", "leading_edge_only" };
    const std::vector<std::string> brim_labels = { "Auto", "Mouse ear", "Painted", "Outer brim only", "Inner brim only",
                                                   "Outer and inner brim", "No-brim", "Leading edge only" };
    const std::vector<std::string> iron_values = { "no ironing", "top", "topmost", "solid" };
    const std::vector<std::string> iron_labels = { "No ironing", "All top surfaces", "Topmost surface only", "All solid layers" };
    const std::vector<std::string> supp_values = { "normal(auto)", "tree(auto)", "normal(manual)", "tree(manual)" };
    const std::vector<std::string> supp_labels = { "Normal (auto)", "Tree (auto)", "Normal (manual)", "Tree (manual)" };

    std::printf("a choice setting\n");
    check("a stored value is taken as it is", resolve_choice(brim_values, brim_labels, "outer_only") == "outer_only");
    check("the customer's case: outer_brim means Outer brim only",
          resolve_choice(brim_values, brim_labels, "outer_brim") == "outer_only", resolve_choice(brim_values, brim_labels, "outer_brim"));
    check("a label is understood: Outer brim only", resolve_choice(brim_values, brim_labels, "Outer brim only") == "outer_only");
    check("whatever its case and spacing: outer-brim_ONLY", resolve_choice(brim_values, brim_labels, "outer-brim_ONLY") == "outer_only");
    check("the same evening's other case: top surfaces is ironing top",
          resolve_choice(iron_values, iron_labels, "top surfaces") == "top", resolve_choice(iron_values, iron_labels, "top surfaces"));
    check("a stored value wins over a part: top is top, not topmost", resolve_choice(iron_values, iron_labels, "top") == "top");
    check("All top surfaces, as labelled, is top", resolve_choice(iron_values, iron_labels, "All top surfaces") == "top");
    check("No-brim, no_brim and no brim are one choice", resolve_choice(brim_values, brim_labels, "No brim") == "no_brim");
    check("outer could be two choices, so it is none", resolve_choice(brim_values, brim_labels, "outer").empty(),
          resolve_choice(brim_values, brim_labels, "outer"));
    check("inner could be two choices, so it is none", resolve_choice(brim_values, brim_labels, "inner").empty(),
          resolve_choice(brim_values, brim_labels, "inner"));
    check("surface could be two ironing choices, so it is none", resolve_choice(iron_values, iron_labels, "surface").empty(),
          resolve_choice(iron_values, iron_labels, "surface"));
    check("tree could be auto or manual, so it is none", resolve_choice(supp_values, supp_labels, "tree").empty(),
          resolve_choice(supp_values, supp_labels, "tree"));
    check("tree(auto) written with a space is still tree(auto)", resolve_choice(supp_values, supp_labels, "Tree (auto)") == "tree(auto)");
    check("two letters are too few to guess from", resolve_choice(brim_values, brim_labels, "ou").empty());
    check("nonsense is none", resolve_choice(brim_values, brim_labels, "skirt").empty());
    check("an empty value is none", resolve_choice(brim_values, brim_labels, "").empty());
    check("it never answers with the default", resolve_choice(brim_values, brim_labels, "force_brim") != "auto_brim");

    std::printf("a per-filament setting\n");
    const std::size_t no = kNotOnPlate;
    check("one figure is for the open filament", figure_for_open_filament(1, 2, 1, 1, true) == 0);
    check("the customer's case: 80,60 with filament 2 open uses the second figure",
          figure_for_open_filament(2, 2, 1, 1, false) == 1);
    check("and with filament 1 open, the first", figure_for_open_filament(2, 2, 1, 0, false) == 0);
    check("four filaments, the third open", figure_for_open_filament(4, 4, 1, 2, false) == 2);
    check("a list that fits neither the plate nor the filament is refused", figure_for_open_filament(3, 2, 1, 0, false) == kNoFit);
    check("unless every figure agrees", figure_for_open_filament(3, 2, 1, 0, true) == 0);
    check("the open filament is not on the plate: refused when the figures differ",
          figure_for_open_filament(2, 2, 1, no, false) == kNoFit);
    check("and taken when they agree", figure_for_open_filament(2, 2, 1, no, true) == 0);
    check("one per nozzle variant of a two-variant filament is used as sent",
          figure_for_open_filament(2, 3, 2, 0, false) == kWholeList);
    check("plate size wins when it equals the variant count", figure_for_open_filament(2, 2, 2, 1, false) == 1);
    check("nothing sent is refused", figure_for_open_filament(0, 2, 1, 0, true) == kNoFit);
    check("a single filament plate", figure_for_open_filament(1, 1, 1, 0, true) == 0);

    std::printf("\n%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
