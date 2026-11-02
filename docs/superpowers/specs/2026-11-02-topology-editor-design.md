# Topology editor: lay out the system by hand

Date: 2026-11-02. Status: approved in conversation, awaiting review of this
document.

## Why

Today the layout — which panels form which string, which strings feed which
MPPT, which MPPTs belong to which inverter — comes from Tigo: a CCA sync (HTTP
or BLE), a Tigo cloud import, or a node-table JSON someone edited by hand and
imported. The Topology page can only rename inverters and strings and set a
string's panel rating.

That leaves two groups stuck:

- **Pi users with no Tigo source.** The Pi has no YAML, so without a sync
  there is no way to create inverters, strings or panel names at all, short of
  hand-editing an exported JSON file and importing it.
- **Anyone fixing a layout.** Moving one panel to another string, or renaming
  panels, means editing that JSON too.

The CCA on the reference site is gone (fried 2026-10-29) and the Pi is the
bus master, so "just sync from the CCA" is no longer always available.

## Goals

1. Build a full layout on the Topology page with no Tigo data: create
   inverters, MPPTs and strings, and put panels into strings by drag and drop.
2. Fix an existing layout the same way: move panels between strings, reorder
   them, rename inverters/MPPTs/strings, delete empty ones.
3. Works on both the Pi (`tigomonitord`) and the ESP32 (`tigo_server`). The
   page is the shared `components/tigo_server/web/app.html`.
4. A layout made by hand is never silently overwritten by a sync, an import,
   or (ESP32) the YAML. The user is asked.

## Non-goals

- No change to how panels are discovered. A panel appears once the bus has
  given its barcode (Frame 27); the editor only places panels the node table
  already has.
- No physical roof map (panel x/y positions). This is the electrical tree only.
- Not a replacement for the Node Table page or its import/export, which stay.
- No multi-user editing or locking beyond "last Save wins".

## Anti-goals (would make it a failure even if it works)

- A stray touch or scroll moving a panel. Editing only happens inside an
  explicit edit mode.
- A sync quietly undoing an hour of hand layout.
- Home Assistant losing history because a panel was renamed or moved. Entity
  identity stays keyed on the barcode (Pi MQTT already uses
  `tigo_<barcode>_<key>`; ESP32 sensors are bound by `sensor_index`, which the
  editor never changes).
- Existing CCA/cloud users noticing any difference until they choose to edit.

## How it behaves (decided with the user)

### Edit mode (option A from the mockups)

The Topology page looks and works exactly as today until the user clicks
**Edit layout**. In edit mode:

- An **Unassigned** tray sits above the inverters: every node-table panel not
  in any string. It is shown only in edit mode.
- Panels get drag handles and can be dragged: tray → string, string → string,
  within a string (reorder), and string → tray.
- **+ Inverter**, **+ MPPT** (inside an inverter) and **+ String** (inside an
  MPPT) buttons appear. Inverter, MPPT and string names are editable in place.
  Empty inverters, MPPTs and strings can be deleted.
- Live readings (watts etc.) keep updating on the panel cards while editing.
- **Save layout** sends the whole edited layout in one request. **Cancel**
  discards every change. Nothing is written before Save.
- Leaving the page or reloading with unsaved changes asks first.

### Panel names follow position

A panel's name is its string's name plus its 1-based position: the third
panel in string "A" is "A3". Reordering renumbers the string. Names are not
edited one by one.

*Calls made while writing this up (flagged as guesses in conversation):*

- New strings default to the next free letter (A, B, C …); new MPPTs to the
  next free "MPPT n", numbered across the whole site as today.
- A string keeps the label it was created with as its identity (what nodes
  carry in `cca_string_label`, and what per-string peaks and history are keyed
  on). Renaming a string sets its display label — the existing
  `display_label` mechanism — and panels are named from the display label
  ("East" → East1, East2 …). So a rename never orphans a string's history.
- In the normal (non-edit) view, panels in no string keep showing where they
  do today.

### When something else disagrees: ask

Once a layout has been saved from the editor, the layout is **manual**.
After that, anything that would change it is held, not applied:

- a CCA sync (HTTP or BLE, scheduled or button),
- a Tigo cloud import,
- a node-table import that carries labels,
- (ESP32) an `inverters:` block in YAML that differs from the saved layout.

If the incoming layout matches the current one, nothing is shown. Otherwise
the page shows a banner ("The CCA's layout differs from yours — review")
with what would change, and two choices: **Keep mine** (drop the incoming
layout) and **Use theirs** (apply it; the layout stops being manual and
behaves as it does today). Nothing changes until the user picks.

*Call made while writing this up:* **Keep mine** remembers a fingerprint of
the layout it turned down, so the same incoming layout (say, a daily CCA sync
that never changes) does not ask again. A different one does.

Before any editor Save, nothing changes: syncs and imports apply as they do
today. That is how existing users see no difference.

## Data model — what exists, and what changes

The editor writes into the structures that already exist; it adds a "manual"
flag, a held-incoming slot, and (ESP32) somewhere to keep inverters that did
not come from YAML.

| Level | Pi today | ESP32 today |
|---|---|---|
| Inverter → its MPPT labels | `topology.json` `inverters[]` (`linux/src/topology.*`); set from the Tools form via `POST /api/topology` | YAML `inverters:` → `add_inverter()`; display name in NVS key `inv_dn:<name>` |
| String → its MPPT | each node's `cca_mppt_label`; strings are derived by grouping nodes on `cca_string_label` (`Monitor::strings_locked_`) | same, `NodeTableData`/`StringData` |
| Panel → string, panel name | node table `cca_string_label`, `cca_label` (persisted with the node table) | `NodeTableData` in NVS slots `node_<i>` |
| String display name, rating | `topology.json` `strings{}` | NVS, via `/api/strings/rename`, `/api/strings/rating` |

Implications the builder must keep:

- A panel's place is still expressed as node-table labels
  (`cca_string_label`, `cca_mppt_label`, `cca_label`). Every other page,
  `/api/strings`, MQTT and history already read those; the editor sets them
  rather than inventing a parallel store.
- Panel order within a string is carried by the position number in the name
  ("A3"), so it survives reboots without a new field. Sort a string's panels by
  that number when rendering.
- An MPPT with no strings yet, and a string with no panels yet, have no node
  carrying their label, so today they would vanish. They must persist: on the
  Pi in `topology.json`, on the ESP32 in NVS alongside the inverter list.
- ESP32: a manual layout's inverter list lives in NVS and, when present, is
  used instead of YAML's `inverters:` at boot. A YAML list that differs is
  held as incoming (see above), never applied silently.

## API

One new endpoint pair on both builds, with the same JSON shape:

- `GET /api/layout` — the full tree: inverters (name, display name) → MPPTs
  (label) → strings (label, display label, rating) → panels (barcode, node
  addr, name, position), plus `unassigned[]` panels, `manual: bool`, and
  `pending` (null, or `{source, summary}` when something incoming is held).
- `POST /api/layout` — replace the whole layout in one request. Validated
  server-side (unique inverter names, an MPPT under only one inverter, unique
  string labels, every panel at most once, every panel known to the node
  table). Panels are identified by **barcode** (`long_address`), not short
  address: short addresses can change under a panel (#74), barcodes don't.
  All-or-nothing: on any error nothing is written. Sets `manual`.
- `GET /api/layout/pending` — the held incoming layout as a diff against the
  current one (panels that would move or be renamed; inverters/MPPTs/strings
  that would be added or removed).
- `POST /api/layout/pending` with `{"action": "keep"}` or `{"action": "use"}`.

Existing endpoints stay and keep working (`/api/inverters`, `/api/strings`,
`/api/inverters/rename`, `/api/strings/rename`, `/api/strings/rating`, Pi
`/api/topology`). The editor does not need them.

`/api/layout` is a new JSON shape, so `site/screenshots/fixtures.mjs` gains it
in the same commit (CLAUDE.md rule). Existing shapes are unchanged.

Auth is as for every other write endpoint on each build (Pi: web login +
cross-site refusal; ESP32: `api_token`).

## Constraints

- **ESP32 memory.** Build responses in PSRAM (`PSRAMString`); use PSRAM
  containers; no per-loop string allocation. A `POST /api/layout` body for 40
  panels is a few KB — read it into PSRAM. Writes to NVS happen once per Save,
  not per drag.
- **ESP32 flash wear.** One Save rewrites the node slots that changed and the
  layout key(s), nothing more.
- **ESP32 NVS sizing.** The inverter/MPPT/string list must fit NVS entries;
  the builder picks the encoding and states the limits (e.g. max inverters,
  MPPTs, strings) in the UI and the API's validation errors.
- **ESP32 without the web server** (no-PSRAM boards) are unaffected: no
  `tigo_server`, no editor; YAML stays the only source there.
- **Pi:** `topology.json` written with `write_file_atomic`, as today. The
  node table save goes through the monitor's existing save path.
- **Pi standby/listen roles:** editing is local display data only; nothing
  goes on the RS485 bus. It must not touch bus-master code.
- **Held incoming layout** is kept in memory. Losing it on a reboot is
  acceptable: the next sync produces it again. (Builder's call.)
- **UI:** `app.html` is one file shared by both builds; no build step, no
  external libraries (HTML5 drag-and-drop or pointer events, written inline).
  Must work with touch (phones and tablets) as well as a mouse. Run
  `node --check` on its inline script after edits.
- **Tests:** Pi side gets unit tests for the layout validation and the
  manual/pending rules, plus an e2e check of `POST /api/layout` →
  `/api/strings` reflecting it. ESP32 side must compile (esp32.yml).

## Left to the builder

- Exact visuals within the current Topology look (see the edit-mode mockup).
- Drag implementation details and touch handling.
- The diff presentation for the pending banner.
- NVS encoding on the ESP32 and its limits.
- Whether the Pi's Tools-page inverter form stays, or points to the editor.

## Branch (decided)

Built on `feature/cca-replacement` (branch `feature/topology-editor` off it),
because the Topology page and both HTTP servers differ heavily between it and
`main` (~760 changed lines in `app.html`, `http_server.cpp`, `monitor.cpp`).
It ships when that branch does.
