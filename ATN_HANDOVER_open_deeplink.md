# Handover — `atnslicer://` deep link: open a farm part on THIS PC

## Goal

Let someone browse the printer farm (farm.askthenozzle.com) on **any** computer,
click **Open** on a part's `.3mf`/gcode, and have it open in **ATN Slicer on the
computer they're using** — then **Send to Farm** writes a new revision back.

Before this, Open always routed through the farm **agent**, so the file opened on
the one machine running the agent, regardless of who clicked. The cloud half is
done; this handover is the **ATN Slicer half**.

---

## What is ALREADY done (cloud side — built, deployed, live)

All in the `printer-farm` repo, deployed to farm.askthenozzle.com. You do not
need to change any of it; it's here so you know the exact contract.

**The Open button** now (for gcode/project) POSTs to
`/parts/{part_id}/revisions/{rev_id}/open-link/{kind}` and gets back a deep link,
then does `window.location.href = deeplink`. The OS routes that to ATN Slicer.
(`app/templates/parts/detail.html`, function `openSlot`.)

**The deep link** the browser hands the OS (`app/slicer_open.py::build_deeplink`):

```
atnslicer://open?url=<ENCODED download url>&kind=<project|gcode>&part=<PART_NUMBER>&rev=<REV_LABEL>&name=<FILENAME>&farm=<https base>
```

Example (url-decoded):
```
atnslicer://open?
  url  = https://farm.askthenozzle.com/api/slicer/open-file?t=<signed-token>
  kind = project
  part = GMR-C-00069
  rev  = B
  name = airbox.3mf
  farm = https://farm.askthenozzle.com
```

**The download endpoint** `GET /api/slicer/open-file?t=<token>`
(`app/routes/slicer.py`):
- Public (no login) — the slicer fetches it headlessly. `/api/slicer` is already
  a `PUBLIC_PATHS` prefix.
- The `t` token is a signed, **5-minute**, single-file token
  (`itsdangerous`, salt `atn-slicer-open-v1`, over `{rev, kind, tid}`). Bad or
  expired → `401 "invalid or expired open link — click Open again"`.
- On success returns the file with a `Content-Disposition` filename (also passed
  redundantly as `&name=` in the deep link, so you can use either).

**Scheme name** is `atnslicer` (overridable server-side via env
`ATN_SLICER_SCHEME`, but treat `atnslicer` as fixed for the installer).

> Verify the cloud half right now: click **Open** on a `.3mf` in the farm — your
> browser shows an "open atnslicer://…" prompt. That proves the link is minted
> and offered. It just needs the slicer to answer it.

---

## What to BUILD (this session — ATN Slicer / OrcaSlicer fork)

Two hooks. The machinery already exists for `orcaslicer://`; you're adding a
sibling scheme with one custom action.

### 1. Register the `atnslicer://` scheme (Windows)

`src/slic3r/GUI/GUI_App.cpp` already calls `associate_url(L"orcaslicer")` in two
places: **line ~2922** (first-run / settings) and **line ~8090**. Add
`associate_url(L"atnslicer")` right beside each. That writes the
`HKCU\Software\Classes\atnslicer\shell\open\command` registry entry pointing at
the exe with `"%1"`. (Confirm `associate_url` is generic over the scheme name —
it is; it takes the protocol as its arg.)

Also add the scheme to the NSIS/installer protocol registration if the installer
writes these at install time (search the installer for `orcaslicer` URL protocol
keys and mirror them for `atnslicer`).

### 2. Handle `atnslicer://open?...`

When the OS launches the exe with `atnslicer://open?...`, it arrives as an input
arg / single-instance message. Trace the existing path:
- `GUI_App.cpp:776` — `is_supported_open_protocol(first_url)` then
  `start_download(first_url)` (this is how `orcaslicer://` project links open).
- `GUI_App.cpp::MacOpenURL` (~line 8561) — macOS entry (do Windows first).
- `InstanceCheck.cpp` `handle_message` (~line 659 in GUI_App) — a second launch
  forwards its args to the running instance.

Add an ATN branch **before** the generic `start_download`: if the URL scheme is
`atnslicer` and host is `open`, do the ATN flow instead:

1. **Parse** query params: `url`, `kind`, `part`, `rev`, `name`, `farm`
   (url-decode each).
2. **Download** `url` to a stable local path. Reuse the same cache convention the
   agent uses so behaviour matches: a per-part folder, filename = `name`.
   (Reference implementation: `printer-farm/agent/agent.py::_write_and_drop` —
   it writes the bytes, writes the sidecar, done. Copy its logic.)
3. **Write the `.farmctx.json` sidecar** next to the downloaded file (see
   contract below). THIS IS WHAT MAKES SEND-TO-FARM WORK.
4. **Open** the file as a project (`request_open_project(local_path)` for `.3mf`,
   or the gcode-open path for gcode).

Keep it robust: network failures / a `401` (expired token) should surface a
clear message ("This open link expired — click Open again in the farm"), not a
silent no-op.

---

## The `.farmctx.json` contract (do NOT get this wrong)

`SendToFarmDialog::load_opened_context()`
(`src/slic3r/GUI/SendToFarmDialog.cpp:331`) reads a sidecar named
**`<opened-3mf-path>.farmctx.json`** and pulls:

```json
{ "part_number": "GMR-C-00069", "rev_label": "B", "farm_url": "https://farm.askthenozzle.com" }
```

- It reads `part_number` and `farm_url` (used to pre-fill the Send-to-Farm dialog
  so a save-back becomes the NEXT revision of the right part). `rev_label` is
  carried for completeness.
- Path is literally the project path **plus** `.farmctx.json` (e.g.
  `…/airbox.3mf.farmctx.json`). If the file isn't at that exact path, the dialog
  falls back to parsing `"<PN> - name.3mf"` from the filename, which won't match
  a plain `airbox.3mf` — so the sidecar is what you rely on.
- Map deep-link params → sidecar: `part` → `part_number`, `rev` → `rev_label`,
  `farm` → `farm_url`.

The agent already writes exactly this (`agent/agent.py`, two call sites of
`path + ".farmctx.json"`), so you're reproducing a known-good format.

---

## Test plan

**Cloud half (already testable):** click Open in the farm → browser offers the
`atnslicer://` link. Copy it and inspect the params.

**After your build:**
1. `associate_url` ran — check `HKCU\Software\Classes\atnslicer` exists.
2. From a browser on a DIFFERENT PC than the agent, click Open on a `.3mf` →
   confirm it opens in ATN Slicer on THAT PC.
3. Confirm the `.farmctx.json` sidecar was written and Send-to-Farm pre-fills the
   part number + farm URL, and a save-back lands as the next revision.
4. Expired token: wait >5 min after minting, click the stale link → clean error,
   not a crash or blank slicer.
5. No-slicer PC: the browser link does nothing (expected). The farm page still
   offers **Download** as the fallback for those.

---

## Gotchas / notes

- **Single-instance**: the common case is the slicer is already running. The
  `atnslicer://open` must be handled on the forwarded-message path
  (`InstanceCheck` → `handle_message`), not only on cold start — test with the
  slicer already open.
- **Don't reuse `orcaslicer://`** — a co-installed stock OrcaSlicer could grab
  the link. Dedicated `atnslicer://` was a deliberate decision.
- **CAD is out of scope** — `.SLDPRT`/CAD still opens via the agent/host path on
  the farm side; the deep link is gcode/project only. The farm refuses to mint an
  open-link for `kind=cad`.
- **Token is single-file, 5 min** — plenty for the OS handoff + download. If a
  large file download risks exceeding it, note that the token is checked once at
  the start of the download, not continuously, so a slow download is fine; only a
  slow *click-through* would expire it.
- **This repo is the ATN fork** (`atn` remote). Land the change here and cut a
  release + installer the usual way — the cloud side is already live, so the
  first real end-to-end test is a fresh installer that registers `atnslicer://`.

## Key references

Cloud (printer-farm, for context only — already deployed):
- `app/slicer_open.py` — token + deeplink builder
- `app/routes/slicer.py` — `GET /api/slicer/open-file`
- `app/routes/parts.py` — `POST …/open-link/{kind}` (`open_revision_deeplink`)
- `app/templates/parts/detail.html` — `openSlot()`
- `agent/agent.py::_write_and_drop` — reference download+sidecar+open behaviour

Slicer (this repo, to change):
- `src/slic3r/GUI/GUI_App.cpp` — `associate_url` (~2922, ~8090);
  `is_supported_open_protocol`/`start_download` (~776); `MacOpenURL` (~8561)
- `src/slic3r/GUI/InstanceCheck.cpp` — single-instance message handoff
- `src/slic3r/GUI/SendToFarmDialog.cpp:331` — `load_opened_context()` (the sidecar reader)
- `src/slic3r/GUI/AtnPanel.cpp` — the ATN panel (unrelated, but nearby)
