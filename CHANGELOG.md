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
