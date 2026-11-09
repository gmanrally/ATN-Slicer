<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="resources/images/atn_about_dark.png">
  <img alt="ATN Slicer logo" src="resources/images/atn_about.png" width="28%">
</picture>

# ATN Slicer

**A next-generation 3D print slicer with a built-in AI print assistant and structural analysis — from [Ask The Nozzle](https://askthenozzle.com).**

ATN Slicer is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer): everything you already know about Orca, plus an in-app AI print doctor, a pre-flight advisor that catches problems *before* you print, and an experimental structural &amp; thermal analysis engine.

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

**[Website](https://askthenozzle.com) · [Download](https://askthenozzle.com/slicer)**

</div>

# What makes it ATN

Underneath, ATN Slicer *is* OrcaSlicer — same profiles, same slicing engine, same workflow — with a layer of intelligence built in:

- **🤖 AI Print Assistant** — an in-slicer chat that reads your model and settings and helps you diagnose failures, choose materials, and tune a print. Ask it *"why did this warp?"* or *"what should I change for PA-CF?"* and get answers grounded in your actual slice, not generic advice.
- **✅ Pre-flight Advisor** — before you print, ATN checks your sliced G-code against a growing knowledge base of real-world failure modes (bed type vs. brim, thermal limits, support choices) and flags issues while they're still cheap to fix.
- **🧪 Structural &amp; Thermal Analysis** *(experimental)* — analyse a sliced part and highlight where it is likely to fail under load — layer-adhesion / delamination and over-stressed regions — using *this print's* actual thermal history and material data rather than a datasheet guess. This is early and actively improving.
- **🧵 Material Intelligence** — per-filament mechanical data with provenance, so advice and analysis reflect the filament you are actually running.

Everything else is OrcaSlicer:

- Advanced calibration — temperature towers, flow rate, pressure advance, retraction, and more
- Precise wall &amp; seam control, sandwich mode, polyholes
- Tree supports, overhang &amp; support optimisation
- Network printing — Klipper, PrusaLink, OctoPrint, Bambu Lab, Creality, and more
- Wide printer compatibility — Bambu Lab, Prusa, Creality, Voron, and many others

# Tiers

- **The slicer is free and open source.** Everything OrcaSlicer does, ATN Slicer does — at no cost.
- **The AI features are account-based and tiered.** New accounts get free credits to try the assistant; heavier use and the pre-flight advisor sit on paid tiers.
- **Structural &amp; thermal analysis** is available on the top tier.

Create an account and see current tiers at **[askthenozzle.com/slicer](https://askthenozzle.com/slicer)**.

# Download

📥 **[Download ATN Slicer](https://askthenozzle.com/slicer)** — Windows installer.

The app updates itself from within: **Help → Check for updates**.

# Building from source

ATN Slicer builds exactly like OrcaSlicer. Follow the [OrcaSlicer build guide](https://www.orcaslicer.com/wiki/how_to_build) for Windows, macOS, or Linux.

> The AI assistant, pre-flight advisor, and structural-analysis features are services provided by Ask The Nozzle and require an account. The open-source build gives you the full slicer plus the client side of those features.

# Klipper note

If you're running Klipper, it's recommended to add the following to your `printer.cfg`:

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Built on OrcaSlicer — and the shoulders it stands on

Open-source slicing has always been built on a tradition of collaboration and attribution, and ATN Slicer is proud to be part of that chain.

[Slic3r](https://github.com/Slic3r/Slic3r), created by Alessandro Ranellucci and the RepRap community, laid the foundation. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research built on Slic3r; [Bambu Studio](https://github.com/bambulab/BambuStudio) forked from PrusaSlicer; and [SuperSlicer](https://github.com/supermerill/SuperSlicer) by @supermerill extended it further. [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) drew from BambuStudio, PrusaSlicer, and others to become the most widely used and actively developed open-source slicer in 3D printing.

**ATN Slicer is a fork of OrcaSlicer.** The entire slicing engine, UI, and printer ecosystem are OrcaSlicer's work — ATN adds the AI assistant, pre-flight advisor, and analysis engine on top. Enormous thanks to [SoftFever](https://github.com/SoftFever) and the OrcaSlicer community, and to every project that came before.

# License

- **ATN Slicer** is licensed under the GNU Affero General Public License, version 3 (AGPL-3.0) — the same license as OrcaSlicer.
- The **AGPL v3** ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.
- ATN Slicer includes a **pressure advance calibration pattern test** adapted from Andrew Ellis' generator, licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, licensed under GNU General Public License, version 3.
- The **Bambu networking plugin** is based on non-free libraries from Bambu Lab. It is optional and provides extended functionality for Bambu Lab printer users.
