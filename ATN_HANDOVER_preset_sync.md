# Handover: push the user's presets to the farm

Written 2026-10-15 from the farm/cloud side, for the ATN Slicer session. The
farm half is built, deployed and tested (printer-farm `7cca666`). What remains
is the slicer half, which is yours.

**The goal**, in the product owner's words: presets a person works with on their
desk should be the presets offered when they slice in the browser at
`farm.askthenozzle.com/slice` — and, later, enough for a works order to re-slice
a part with nobody choosing anything.

Today they have to export a folder and upload it by hand.

---

## What already exists (nothing here needs changing)

**The channel.** This slicer already authenticates to the farm. `farm_token()`
reads `app_config->get("farm", "token")`; `add_token()` sends it as
`X-Farm-Token`; `farm_url()` resolves `FARM_URL` env → the persisted field →
`https://farm.askthenozzle.com`. All three are in `SendToFarmDialog.cpp` and
are what the new calls should use. No new auth, no new sign-in.

**The destination.** The farm stores presets per tenant and the cloud slicer
already reads them. The token carries the tenant, so you never send one.

**Two endpoints**, both requiring the token (401 without it):

```
GET  /api/slicer/presets/sync
  -> {"ok":true,"presets":[{"kind":"process","name":"PA6-CF fine",
                            "sha256":"<of the stored json>"}, ...]}

POST /api/slicer/presets/sync
  {"presets":[ <orca preset json>, ... ]}
  -> {"ok":true,
      "synced":[{"kind":"process","name":"PA6-CF fine"}],
      "skipped":[{"name":"0.20mm Standard @Creality","reason":"built-in preset"}]}
```

`kind` is `machine` | `filament` | `process`. Max 500 presets per POST.

---

## The one trap that will cost you an afternoon

**Post the preset's on-disk JSON verbatim. Do not re-serialise from the
config.**

The farm decides what is "the user's" rather than the vendor's, and it does so
by inspecting fields that only exist in the file:

- `from == "system"`, or any `instantiation` key → rejected as a built-in.
- `model_id` with no `inherits`, no `printable_area`, no `printer_settings_id`
  → rejected as a printer *model definition*, not a preset.
- Something that neither sets a real setting nor inherits anything → rejected.

A preset rebuilt from `DynamicPrintConfig` loses `from`, `inherits` and
`is_custom_defined`, so it will either be refused or stored as something the
CLI cannot resolve. `Preset::file` is the path; read those bytes and send them.

The reply names every rejection and why, so log `skipped` rather than only a
count — that is the difference between "3 presets didn't sync" and knowing
which.

---

## What to build

### 1. Enumerate the user's presets

`wxGetApp().preset_bundle` gives you the three collections, which map to the
farm's `kind`:

| PresetBundle member | farm `kind` |
| --- | --- |
| `printers` | `machine` |
| `filaments` | `filament` |
| `prints`    | `process` |

Iterate and keep only a person's own: skip `is_default`, skip `is_system`, and
skip `is_external` (a preset loaded from a project file is not part of their
library). `begin()` on a `PresetCollection` already skips the default presets,
which is a start but not the whole filter.

For each survivor, read `Preset::file` and parse it as JSON. If the object has
no `"type"`, set it from the collection it came from — the farm infers when it
can, but telling it is free and removes a guess.

### 2. Send only what changed

`GET` first, build a map of `(kind, name) -> sha256`, and POST only presets
whose file hash differs or whose `(kind, name)` is absent. The farm hashes the
JSON text it stored, so compare against the same bytes you would send.

Without this, every sign-in re-uploads the whole library. With ~80 presets
that is a few MB each time, on a slicer that may be on a laptop tether.

### 3. Decide when it runs

Suggested, but your call:

- on successful sign-in to the farm,
- when a preset is saved (there is already a save path to hook), debounced,
- and a manual **Sync presets now** action, because people want to see it work.

### 4. Make it opt-in and visible

The owner asked for this as **an option**. A checkbox near the farm settings
("Sync my presets to this farm"), persisted in `app_config` under the `farm`
section beside `token`, defaulting **off**. Someone whose farm is shared with
colleagues should choose to push their presets into it.

Surface the result: "42 presets synced, 3 skipped" with the reasons on hover.

---

## Rules the farm side already enforces, so you can rely on them

- **Additive.** Sync adds and updates; it never deletes. Deleting a preset here
  does not remove it there. This is deliberate — a reinstall or a second PC on
  the same account would otherwise prune presets a colleague is slicing with.
  Do not try to emulate deletion by sending a shorter list; nothing will happen.
- **Upsert on (tenant, kind, name).** Re-sending a preset updates it in place,
  so the sync is safe to repeat and safe to retry after a network failure.
- **Kind is part of the key**, so a process and a machine preset may share a
  name without colliding.
- **Tenant comes from the token.** A preset can only ever land in the farm the
  slicer is signed in to. (Tested: removing that filter fails the suite.)

---

## Testing

Against a local farm, `FARM_URL=http://127.0.0.1:8000` and the local farm
ignores the token. Against the real one you need a signed-in build.

A round trip worth doing before you call it done:

1. Create a distinctive process preset on the desktop (`ZZTEST-sync-me`).
2. Sync. Confirm `synced` names it.
3. Open `farm.askthenozzle.com/slice` and confirm it appears in the Process
   picker, and that slicing with it works — the stored JSON has to be
   resolvable by the headless engine, not merely stored.
4. Edit a value, sync again, confirm the change arrives and no duplicate row
   appears.
5. Sync twice with no changes and confirm the second sends nothing (step 2 of
   "what to build" is working).

Name test presets `ZZTEST-` so they are obvious in the farm's profiles list.

---

## What this unblocks, and what is NOT yours

Revisions now record the preset triple they were sliced with
(`slice_machine_preset`, `slice_process_preset`, `slice_filament_preset`, plus
overrides), captured when a cloud slice is sent into the farm. That is the
other half of unattended re-slicing, and it is farm-side — already done.

Still open on the farm side, not this session: nothing yet *reads* that triple,
so a works order cannot act on it. That needs a rule for parts with no recorded
setup, and it is being picked up separately.

Questions about the farm end: the endpoints are in `printer-farm`
`app/routes/slicer.py` (`slicer_presets_sync`), the storage and the
accept/reject predicate in `app/routes/cloud_slice.py` (`upsert_preset`,
`_not_a_user_preset`), and the behaviour is pinned by
`tools/test_preset_sync.py`.
