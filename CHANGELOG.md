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

### Added
- **The print diary records prints sent straight to your printer.** A job the slicer sends to a
  printer now appears in the assistant's **Learning** tab (Workshop), the same as a job sent to
  the farm, so you can mark how it turned out. That covers an upload through any print host
  (Klipper / Moonraker, OctoPrint, PrusaLink, FlashForge, Creality, Elegoo, Qidi and the rest) and
  a Bambu print or send. It is recorded once the file has arrived, so a failed upload leaves
  nothing behind. Until now only farm jobs were recorded, so anyone printing directly had nothing
  to mark. Calibration prints are not recorded, and a file exported to disk or a USB stick is not
  either: the slicer cannot tell whether it was printed.
- **Nozzle sizes dialog (H3C, H2C, H2D / X2D).** The in-app selector for mixed nozzle sizes
  that 2.7.0 left "to come": a **Nozzle sizes** button in the filament bar (shown for Bambu
  printers with a hotend rack or two extruders) opens a dialog to switch *Mixed nozzle sizes
  in this project* on, record the nozzle fitted in each slot (diameter and flow type, or an
  empty slot), and choose which nozzle each filament prints with. Each filament row in the
  sidebar then shows its nozzle size. A plate in custom grouping gets its per-filament nozzle
  map from the same choices, so a filament can never be sent to a nozzle of another size.
  Everything is saved with the project. Please report what you see on the printer: mixed-size
  prints are verified in slicing only.
- **Read from printer (Nozzle sizes dialog).** One press fills the slot list from the nozzles
  the connected printer reports: size and flow type per slot, empty slots left empty. It
  switches mixed nozzle sizes on when the printer carries more than one size, and says so.
  A tool the printer has not identified as a usable nozzle (an unread hotend, a cutter or a
  pen) is left out and counted in the status line. Nothing is written until you press Apply.
  The device layer now reads the slot address, the present / absent flag and the tool kind a
  rack printer sends for each tool position (as Bambu Studio 2.9.1 does).

## v2.7.1 — 2026-11-18

### Fixed
- Prime tower filament-used accounting: the layer-is-printed check introduced with the
  Bambu Studio 2.9 tower called itself, so on sparse tower layers the filament used by
  the tower depended on the compiler. The Linux (cloud slicer) build hung forever on
  any mixed-nozzle tower; the Windows build printed the right geometry but may have
  misreported the tower's filament use. The upstream rule is restored.
- FlashForge Adventurer 5M, 5M Pro and AD5X printer profiles now default the Physical
  Printer's Host Type to "Flashforge". A new connection used to start on
  "Octo/Klipper", which these printers do not speak: the Device tab showed a browser
  error ("refused to connect") instead of the FlashForge status page, and sending a
  print failed. Physical printers you already saved keep their Host Type.
- Adding a printer that cannot be loaded now says so. When the printer profiles stored
  on this computer are damaged, Confirm in "Select/Remove printers" used to close
  without a word and leave the Printer box empty. The slicer now explains what happened
  and how to repair the copy (rename the "system" folder in its data directory and add
  the printer again).

### Changed
- The ATN assistant panel now reports the ATN release (for example 2.7.0) as the app
  version in the context it sends with each question. It used to send the upstream
  OrcaSlicer base version, which read the same for every ATN build, so a saved
  assistant case could not show which release the user was on.

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
