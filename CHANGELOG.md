# ATN Slicer — Changelog

User-facing changes to ATN Slicer (Ask The Nozzle's OrcaSlicer fork).

How this is used:
- Add a bullet under **Unreleased** whenever a user-visible change lands.
- When cutting a release: bump `version.inc` (`SoftFever_VERSION`), move the
  **Unreleased** bullets under a new `## vX.Y.Z — YYYY-MM-DD` heading, and paste
  that same text into the `body` field of `data/slicer_release.json` on the server
  (that is what the in-app update prompt shows the user).
- Internal-only changes (e.g. anything behind `ATN_FARM_TOOLS`) do NOT belong here —
  they never ship in public builds.

## Unreleased

### Fixed
- Prime tower filament-used accounting: the layer-is-printed check introduced with the
  Bambu Studio 2.9 tower called itself, so on sparse tower layers the filament used by
  the tower depended on the compiler. The Linux (cloud slicer) build hung forever on
  any mixed-nozzle tower; the Windows build printed the right geometry but may have
  misreported the tower's filament use. The upstream rule is restored.

## v2.7.0 — 2026-11-13

### Added
- **Bambu Lab H3C.** The nine-hotend H3C appears in the printer list with Bambu's
  0.2/0.4/0.6/0.8 nozzle machines, 16 process presets and 135 filament presets
  (converted from Bambu Studio 2.9), its bed model and its device capabilities. Tool
  changes use Bambu's hotend remap, the prime tower addresses hotends as tools, the
  dedicated 2.85 mm TPU hotend is a selectable nozzle variant, and retraction state is
  tracked per hotend. Verified on an H3C by a customer during the beta.
- **Mixed nozzle sizes in one print (H3C, H2C, H2D/X2D), ported from Bambu Studio 2.9.**
  With the project's mixed nozzle diameter mode on, each filament is assigned the nozzle
  diameter it is printed with and the slicer derives everything from it: layer heights
  per nozzle (a 0.2 mm hotend prints 0.1 mm layers while a 0.4 mm hotend prints 0.2 mm
  layers in the same print, merged on one layer grid), line widths scaled from the
  preset's nozzle to the printing nozzle, wall counts, top/bottom shells, retraction,
  ironing inset and support gaps per nozzle. Filament grouping knows the hotend rack
  slot by slot (installed nozzles, diameters, the AMS-fed hotend's capacity, the
  2.85 mm TPU hotend) and only places a filament on a nozzle of its diameter. The
  Bambu prime tower is the 2.9 tower: it merges its slabs per nozzle so coarse and
  fine hotends share one tower, with Bambu's side scrape, tail slow-down, lift and
  wipe options. Hotend pre-cooling and pre-heating follow Bambu Studio 2.9 on the rack
  (per-filament pre-heat opt-out, rack cooling rate, idle temperature floor, cooling
  sent before the filament change). Command-line and cloud slicing accept
  `multi_nozzle_diameter_mode` and `filament_diameter_map` in the machine settings;
  the in-app nozzle selector for the mode is still to come. A project saved by Bambu
  Studio 2.9 with mixed nozzles loads and slices here. Mixed-size prints were verified
  in slicing only, so please report what you see on the printer.
- **Everything in OrcaSlicer 2.5.0.** ATN Slicer now tracks upstream 2.5.0: official
  H2C and A2L support, mixed nozzle sizes on the H2D and X2D, Python plugins, belt
  printers, printing several parts at once on IDEX printers, toolhead collision
  volumes, faster startup, slicing and export, Speed Dial, Precise Seam modifiers, the
  wipe-tower placement and estimation fixes, extruder variants on every printer, the
  2.4.1/2.4.2 bug fixes and the refreshed Bambu profile bundle (five more H2C process
  presets, 22 more H2C filaments). Bambu process presets default to Grid infill after
  the upstream profile sync; set Cross Hatch in the preset to keep it.
- **"Send print job" dialog for Qidi printers.** Sending to a Qidi printer (X-Max 4,
  X-Plus 4, Q2 and the rest of the Qidi profiles) now opens the same style of dialog
  Bambu printers get: plate thumbnail, file name, estimated time and weight, the
  printer with its live Klipper state and plate type, the plate's filaments, and the
  four job options Qidi Studio offers — Timelapse, Auto Bed Leveling, Enable Box and
  Enable Polar Cooler. The options are read from the printer when the dialog opens
  and written back over Moonraker just before the upload, so they take effect for
  that print exactly as they do from Qidi Studio. Send starts the print; "Upload only"
  just stores the file.

## v2.4.1 — 2026-06-16

### Fixed
- **Support-interface fan now cools the correct layer.** It targets the interface
  layer that actually contacts the model underside, across all support types
  (Normal/Grid/Snug, Tree Organic, and Tree Slim/Strong/Hybrid) — previously, on
  tree supports it could cool the wrong (lower) interface band.
- **Filament → Cooling tab no longer crashes** when opened.
- **Clone dialog respects the number you type.** "Number of copies: 4" now makes 4
  copies instead of one, and turning on "Auto arrange plate after cloning" no longer
  runs away and floods the bed onto extra plates.

### Added
- **"Top interface layer only" option** for the support-interface fan: cool just the
  model-contacting interface layer (default), or untick to cool all interface layers.
