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
