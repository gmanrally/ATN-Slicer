"""Build + run test_slots.cpp against the real header, then against broken copies.

A rule is only pinned if breaking it makes the test fail, so each mutation below removes one rule of
MachineNozzleSlots::place and the run must then FAIL.  Not part of the CMake test tree (the GUI library has none): needs only g++ and python.
Usage: python run.py ../../../src/slic3r/GUI/MachineNozzleSlots.hpp
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = tempfile.mkdtemp(prefix="nozzle-slots-")  # executables and broken header copies go here, not in the repo
HEADER = sys.argv[1]
SRC = open(HEADER, encoding="utf-8").read()

RACK_LOOP = ("if (tool.extruder == extruder && tool.on_rack)\n"
             "            put(tool.addr_id >= 0 ? tool.addr_id : tool.id, &tool);")
MOUNTED_GUARD = ("if (tool.extruder != extruder || tool.on_rack)\n"
                 "            continue;")

# name, [(old, new), ...]
MUTATIONS = [
    ("ignore addr_id on the rack",
     [("put(tool.addr_id >= 0 ? tool.addr_id : tool.id, &tool);", "put(tool.id, &tool);")]),
    ("ignore addr_id on the extruder",
     [("int slot = tool.addr_id;\n        if (slot < 0) {", "int slot = -1;\n        if (slot < 0) {")]),
    ("a vacant placeholder is not a free slot",
     [("if (!placed[i] || !placed[i]->exists) {", "if (!placed[i]) {")]),
    ("clamp an out-of-range slot",
     [("if (slot >= 0 && slot < int(placed.size()))\n            placed[size_t(slot)] = tool;",
       "placed[size_t(slot < 0 ? 0 : (slot >= int(placed.size()) ? int(placed.size()) - 1 : slot))] = tool;")]),
    ("one pass in report order instead of rack first, mounted on top",
     [(RACK_LOOP,
       RACK_LOOP + "\n        else if (tool.extruder == extruder) {\n"
       "            int slot = tool.addr_id;\n"
       "            if (slot < 0) { slot = 0; for (size_t i = 0; i < placed.size(); ++i) if (!placed[i] || !placed[i]->exists) { slot = int(i); break; } }\n"
       "            put(slot, &tool);\n"
       "        }"),
      (MOUNTED_GUARD, "continue;")]),
    ("an unusable tool reads as a nozzle",
     [("if (!tool->usable || tool->diameter.empty()) {", "if (tool->diameter.empty()) {")]),
    ("a tool with no diameter reads as a nozzle",
     [("if (!tool->usable || tool->diameter.empty()) {", "if (!tool->usable) {")]),
    ("extruders not separated",
     [(MOUNTED_GUARD, "if (tool.on_rack)\n            continue;")]),
    ("a tool reported as absent still fills its slot",
     [("if (!tool || !tool->exists)\n            continue;", "if (!tool)\n            continue;")]),
]


def build_and_run(include_dir, tag):
    exe = os.path.join(WORK, "t_%s.exe" % tag)
    cmd = ["g++", "-std=c++17", "-O0", "-I", include_dir, os.path.join(HERE, "test_slots.cpp"), "-o", exe]
    c = subprocess.run(cmd, capture_output=True, text=True)
    if c.returncode != 0:
        return None, c.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


rc, out = build_and_run(os.path.dirname(os.path.abspath(HEADER)), "real")
print(out)
if rc != 0:
    print("REAL HEADER: FAILED (rc=%s)" % rc)
    sys.exit(1)
print("REAL HEADER: all checks pass\n")

bad = 0
for n, (name, pairs) in enumerate(MUTATIONS):
    src = SRC
    ok = True
    for old, new in pairs:
        if src.count(old) != 1:
            print("MUTATION %d (%s): anchor found %d times, expected 1" % (n, name, src.count(old)))
            ok = False
            break
        src = src.replace(old, new)
    if not ok:
        bad += 1
        continue
    d = os.path.join(WORK, "mut%d" % n)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    open(os.path.join(d, "MachineNozzleSlots.hpp"), "w", encoding="utf-8").write(src)
    rc, out = build_and_run(d, "mut%d" % n)
    if rc is None:
        print("MUTATION %d (%s): did not compile\n%s" % (n, name, out))
        bad += 1
    elif rc == 0:
        print("MUTATION %d (%s): test still PASSES -> rule not pinned" % (n, name))
        bad += 1
    else:
        failed = [l[5:] for l in out.splitlines() if l.startswith("FAIL")]
        print("mutation %d caught (%s): %s" % (n, name, "; ".join(failed)))
print("\n%d mutation(s) not caught" % bad)
sys.exit(1 if bad else 0)
