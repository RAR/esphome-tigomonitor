# Topology Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lay out inverters → MPPTs → strings → panels by drag and drop on the Topology page, on the Pi and the ESP32, with no Tigo data, and never let a sync silently overwrite a hand-made layout.

**Architecture:** The layout rules (validation, panel naming, diff, fingerprint, hold decision, skeleton encoding) live in one header-only file shared by both builds, tested on the host like `tigo_node_identity.h`. Each build stores a panel's place as the node-table labels it already uses, plus a small "skeleton" (inverters, MPPTs, strings — including empty ones) and a `manual` flag; every path that writes labels (CCA sync, cloud import, node import, ESP32 YAML) goes through one `offer` gate that applies or holds. The page gets an edit mode that reads `GET /api/layout` and saves with one `POST /api/layout`.

**Tech Stack:** C++17 (Pi daemon, nlohmann/json, cpp-httplib), ESPHome ESP-IDF C++ (cJSON, esp_http_server, NVS preferences, PSRAM), vanilla JS in `app.html`, Node test runner + Playwright for the page, shell/Python e2e for the Pi.

**Spec:** `docs/superpowers/specs/2026-11-02-topology-editor-design.md`

**Branch:** `feature/topology-editor` (off `feature/cca-replacement`).

## Global Constraints

- Panel identity is the **barcode** (`long_address`), never the short address (#74).
- Panel name = (string display label, else string label) + 1-based position: "A3".
- A string's label is its identity and never changes after creation; renames set `display_label`.
- New strings default to the next free letter A…Z then AA…; new MPPTs to the next free "MPPT n" across the site; new inverters to "Inverter n".
- Every name (inverter name/display, MPPT label, string label/display, generated panel name) ≤ 32 bytes, no `|`, no control characters (the ESP32 node slot is `|`-delimited in 256 bytes).
- Until the first editor Save, every sync/import behaves exactly as today.
- Once `manual`, an incoming layout that differs is held; identical → nothing shown; same fingerprint as the last declined → nothing shown.
- "Keep mine" stores the declined fingerprint persistently; "Use theirs" applies and clears `manual`.
- The held layout itself is in memory only.
- Entity identity unchanged: Pi MQTT `unique_id` stays `tigo_<barcode>_<key>`; the editor never touches `sensor_index`.
- `app.html`: no external libraries; works with touch; `node --check` its inline script after edits.
- Any new `/api/*` JSON shape goes into `site/screenshots/fixtures.mjs` in the same commit; fixture barcodes use the `04C05B0000` fake prefix (enforced by `site/test/fixtures-privacy.test.mjs`).
- ESP32: request bodies and responses in PSRAM; NVS written once per Save, not per drag; `max_uri_handlers` is 60 (51 used today).
- Never touch bus-master code; this is display data only.
- Commit messages: house style (`type(scope): subject`, body with verification numbers), no attribution trailers. CHANGELOG is deferred to the branch's release pass (user's standing instruction); do not edit it.

## Review Focus

1. A panel whose short address changed since the page loaded (renumbering mid-edit) — Save must still place it by barcode. → Task 2 e2e "save after renumber".
2. A periodic CCA sync after "Keep mine" with the same data — must not re-raise the banner, also after a daemon restart. → Task 3 e2e.
3. Save with a panel listed twice, or a barcode the node table doesn't have (a panel deleted in another tab) — 400, nothing written. → Task 1 host test + Task 2 e2e.
4. Two tabs: Save in one, then Save a stale tree in the other — last Save wins, no crash, no duplicate panels. → Task 2 e2e "stale save".
5. Touch drag on a phone, and a scroll that starts on a panel card outside edit mode — only edit mode drags. → Task 6 browser test with touch emulation.

---

### Task 1: Shared layout rules (`tigo_layout.h`)

**Files:**
- Create: `components/tigo_monitor/tigo_layout.h`
- Create: `tests/host/layout_test.cpp`
- Modify: `linux/scripts/build.sh` (add `layout_test` to the host-test list)

**Interfaces:**
- Produces (namespace `esphome::tigo_monitor::layout`, std types only, no ESPHome includes):
  - `struct StringSpec { std::string label, display_label; int rating_w = 0; std::vector<std::string> panels; };` — `panels` = barcodes in order.
  - `struct MpptSpec { std::string label; std::vector<StringSpec> strings; };`
  - `struct InverterSpec { std::string name, display_name; std::vector<MpptSpec> mppts; };`
  - `struct Layout { std::vector<InverterSpec> inverters; };`
  - `struct Assignment { std::string barcode, label, string_label, mppt_label; };`
  - `constexpr size_t kMaxName = 32;`
  - `std::string validate(const Layout &, const std::function<bool(const std::string &)> &known_barcode);` — "" if valid, else a sentence for the user.
  - `std::string panel_name(const StringSpec &, size_t index);`
  - `std::vector<Assignment> assignments(const Layout &);`
  - `int position_of(const std::string &panel_label);` — trailing integer, or `INT_MAX` if none.
  - `struct Change { std::string barcode; Assignment now, then; };` and `std::vector<Change> diff(const std::vector<Assignment> &current, const std::vector<Assignment> &incoming);` — one entry per incoming barcode whose label/string/MPPT differs from current (a barcode absent from current counts as `now` all-empty).
  - `using InverterMppts = std::vector<std::pair<std::string, std::vector<std::string>>>;` — (inverter name, its MPPT labels), the form an incoming inverter list takes on both builds.
  - `uint32_t fingerprint(const std::vector<Assignment> &, const InverterMppts &inverters);` — FNV-1a over the sorted canonical text; order-independent.
  - `enum class Offer { apply, hold, unchanged, declined };` `Offer decide(bool manual, bool has_changes, uint32_t incoming_fp, uint32_t declined_fp);`
  - `std::string encode_skeleton(const Layout &, bool manual, uint32_t declined_fp);` / `bool decode_skeleton(const std::string &, Layout &, bool &manual, uint32_t &declined_fp);` — panels are NOT encoded (they live on the nodes). Line format: `v1|<manual 0/1>|<declined_fp hex>`, then `I|name|display`, `M|label`, `S|label|display|rating`, nested in order. Max encoded size `kMaxSkeleton = 2048`.

- [ ] **Step 1: Write the failing tests** in `tests/host/layout_test.cpp` (same CHECK macro and `main` style as `tests/host/node_identity_test.cpp`; made-up barcodes `04C05B0000000A01`…):
  - `test_panel_names_follow_position`: string label "A", display "" → names "A1","A2","A3"; display "East" → "East1".
  - `test_validate_accepts_a_good_layout`: 1 inverter, 2 MPPTs, 2 strings, 4 panels → `""`.
  - `test_validate_rejects`: each returns non-empty and names the culprit: duplicate inverter name; MPPT under two inverters; duplicate string label; same barcode twice; unknown barcode (`known_barcode` returns false); empty name; a name containing `|`; a generated panel name of 33 bytes (display label of 31 chars + position 10).
  - `test_assignments_carry_the_tree`: panel in inverter "Inv 1" / "MPPT 2" / string "B" (display "West"), position 2 → `{barcode, "West2", "B", "MPPT 2"}`.
  - `test_position_of`: "A3"→3, "East12"→12, "Panel"→INT_MAX, ""→INT_MAX.
  - `test_diff_only_reports_real_changes`: identical lists → empty; one panel moved → one Change with both sides; incoming barcode absent from current → Change with empty `now`; a current barcode absent from incoming → no Change.
  - `test_fingerprint_is_order_independent`: same assignments in two orders → equal; one label changed → different.
  - `test_decide`: not manual → apply (even with changes); manual + no changes → unchanged; manual + changes + fp == declined → declined; manual + changes + new fp → hold.
  - `test_skeleton_round_trips`: encode then decode gives the same inverters/MPPTs/strings (labels, display labels, ratings, order), `manual`, `declined_fp`; panels come back empty; empty MPPTs and empty strings survive; decode of `"garbage"` returns false.

- [ ] **Step 2: Run to verify it fails**
  Run: `docker run --rm -v "$PWD":/src -w /src tigod-dev sh -c 'g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor tests/host/layout_test.cpp -o /tmp/lt && /tmp/lt'`
  Expected: compile error, `tigo_layout.h: No such file or directory`.

- [ ] **Step 3: Implement `tigo_layout.h`** with the interfaces above. Header comment in the style of `tigo_node_identity.h`: why it exists (one set of layout rules for the Pi and the ESP32), and that it is host-tested.

- [ ] **Step 4: Run to verify it passes**, then add `layout_test` to the `for t in …` list in `linux/scripts/build.sh` and run `docker run --rm -v "$PWD":/src -w /src tigod-dev linux/scripts/build.sh`.
  Expected: `all passed` for all five host tests; daemon builds.

- [ ] **Step 5: Commit** — `feat(layout): one set of topology layout rules for the Pi and the ESP32`.

---

### Task 2: Pi — read and save the layout (`GET/POST /api/layout`)

**Files:**
- Modify: `linux/src/topology.h`, `linux/src/topology.cpp` (manual flag, declined fingerprint, skeleton strings with their MPPT, in-memory pending slot)
- Modify: `linux/src/monitor.h`, `linux/src/monitor.cpp` (barcode-keyed assignment)
- Modify: `linux/src/http_server.cpp` (two routes)
- Modify: `site/screenshots/fixtures.mjs` (`/api/layout`)
- Create: `linux/scripts/layout-e2e.sh`
- Modify: `.github/workflows/pi.yml` (run `layout-e2e.sh` after `cca-e2e.sh`)

**Interfaces:**
- Consumes: Task 1 (`Layout`, `validate`, `assignments`, `position_of`).
- Produces:
  - `Topology`: `bool manual() const; void set_manual(bool);` `uint32_t declined_fp() const; void set_declined_fp(uint32_t);` `layout::Layout skeleton() const;` (inverters → MPPTs → strings, no panels) `bool set_skeleton(const layout::Layout &);` (replaces inverters and the MPPT of every skeleton string; keeps ratings/display labels it is given). Persisted in `topology.json` as `"manual"`, `"declined_fp"`, and `strings{label:{…,"mppt"}}`; `save_locked_` must keep a string entry that has an `mppt` even with no display label/rating.
  - `struct PendingLayout { std::string source; std::vector<layout::Assignment> assignments; std::vector<InverterConfig> inverters; uint32_t fp = 0; };` and `Topology::set_pending(PendingLayout)`, `std::optional<PendingLayout> pending() const`, `void clear_pending()` — memory only.
  - `Monitor::set_assignments(const std::vector<layout::Assignment> &, bool clear_unlisted)` — matches nodes by exact `long_address`; sets `cca_label`, `cca_string_label`, `cca_mppt_label`, and each panel's `label`; with `clear_unlisted`, nodes not listed get those three cleared; clears `string_peaks_` and saves. Returns the number of nodes matched.
  - `Monitor::current_assignments() const -> std::vector<layout::Assignment>` (nodes with a barcode).
  - `std::string layout_json(const Monitor &, const Topology &)` (free function in `http_server.cpp` or a new `linux/src/layout_api.cpp` if it exceeds ~80 lines) producing exactly:
    `{"manual":bool,"inverters":[{"name","display_name","mppts":[{"label","strings":[{"label","display_label","rating_w","panels":[{"barcode","addr","name"}]}]}]}],"unplaced_mppts":[{"label","strings":[…]}],"unassigned":[{"barcode","addr","name"}],"pending":null|{"source","changes":N}}`
    Tree = skeleton ∪ what the nodes say (a node's string not in the skeleton is added under its node MPPT; an MPPT under no inverter goes in `unplaced_mppts`; a string with no MPPT goes under an `unplaced_mppts` entry with label `""`). Panels sorted by `position_of(name)`, then barcode. `unassigned` = nodes with a barcode and no string label.
  - `POST /api/layout` body = `{"inverters":[ InverterSpec as JSON, panels as barcode strings ]}`. 400 `{"success":false,"error":…}` on bad JSON or `validate` failure (nothing written); else `set_skeleton`, `set_assignments(assignments(l), true)`, `set_manual(true)`, `clear_pending()`, 200 `{"success":true}`. 500 if `topology.json` can't be written.

- [ ] **Step 1: Write the failing e2e** `linux/scripts/layout-e2e.sh`, modelled on `cca-e2e.sh` (own state dir `build/layout-e2e`, port 18091, `--serial /dev/nonexistent`). Seed nodes with `POST /api/nodes/import` (barcodes `04C05B00000000A1`…`A4`, no labels). Checks:
  - fresh: `GET /api/layout` → `manual` false, 4 in `unassigned`, no inverters.
  - save: POST one inverter "Inverter 1" / "MPPT 1" / string "A" with A3,A1,A2 order, string "B" empty → 200; `GET /api/layout` → `manual` true; string A panels named A1,A2,A3 in the posted order; string B present and empty; one `unassigned` left; `GET /api/strings` has label "A" with mppt "MPPT 1"; `GET /api/nodes` shows `cca_label` "A1" on the first posted barcode.
  - rename: same tree with string A `display_label` "East" → names East1..East3; `/api/strings` label still "A".
  - reject: same barcode twice → 400 and layout unchanged; unknown barcode → 400 and unchanged.
  - save after renumber: re-import the same barcodes on new short addresses, POST again → names land on the barcodes.
  - stale save: POST tree X, POST an older tree Y → layout equals Y, no barcode appears twice.
  - survives restart: kill and restart the daemon on the same state dir → same `GET /api/layout` (string B still there, `manual` true).

- [ ] **Step 2: Run to verify it fails**
  Run: `docker run --rm -v "$PWD":/src -w /src tigod-dev sh -c 'linux/scripts/build.sh >/dev/null && linux/scripts/layout-e2e.sh'`
  Expected: FAIL on the first check (`/api/layout` 404).

- [ ] **Step 3: Implement** the Topology, Monitor and route changes above.

- [ ] **Step 4: Add the fixture** — `'/api/layout'` in `site/screenshots/fixtures.mjs`, built from the fixtures' existing inverters/strings/devices so the Topology screenshot and the editor agree; `manual: false`, `pending: null`, one `unassigned` panel with a `04C05B0000…` barcode.

- [ ] **Step 5: Run to verify it passes** — the e2e from Step 2 (`layout e2e passed`), then `linux/scripts/e2e.sh` and `cca-e2e.sh` still pass, and `cd site && node --test test/*.test.mjs` passes.

- [ ] **Step 6: Commit** — `feat(linux): save a hand-made topology layout`; add the pi.yml step in the same commit.

---

### Task 3: Pi — hold what disagrees (`/api/layout/pending`)

**Files:**
- Modify: `linux/src/monitor.h`, `linux/src/monitor.cpp` (split `apply_layout` into resolve + apply)
- Modify: `linux/src/cca_http.cpp` (offer instead of apply)
- Modify: `linux/src/http_server.cpp` (cloud import, node import, two pending routes)
- Modify: `linux/scripts/layout-e2e.sh`

**Interfaces:**
- Consumes: Task 1 `diff`, `fingerprint`, `decide`; Task 2 `Topology` pending/manual/declined, `Monitor::set_assignments`, `current_assignments`.
- Produces:
  - `Monitor::resolve_layout(const std::vector<LayoutPanel> &) const -> std::vector<layout::Assignment>` — the existing last-6 match, no writes. `apply_layout` becomes resolve + `set_assignments(…, false)` and keeps its signature for the not-manual path.
  - `layout::Offer offer_layout(Monitor &, Topology &, PendingLayout incoming)` (free function next to `layout_json`): computes `diff` against `current_assignments()` (and inverter changes when `incoming.inverters` is non-empty), `decide(...)`; on `apply` → `set_assignments(incoming.assignments, false)` and, if `incoming.inverters` non-empty and current inverters empty, `set_inverters` (today's rule); on `hold` → `set_pending`; logs which. Callers: CCA HTTP sync (`source "cca"`), `/api/cloud/import` (`"cloud"`, with inverters), `/api/nodes/import` (`"import"`: the file's labels as assignments; the node addresses/barcodes themselves are still imported as today — only the labels are offered).
  - `GET /api/layout/pending` → `{"source", "changes":[{"barcode","now":{"name","string","mppt"},"then":{…}}], "inverters_now":[…]|null, "inverters_then":[…]|null}` or `{"pending":null}`.
  - `POST /api/layout/pending {"action":"keep"|"use"}` → keep: `set_declined_fp(fp)`, `clear_pending`; use: `set_assignments(assignments,false)`, inverters if given, `set_manual(false)`, drop skeleton strings that now have no nodes, `clear_pending`. 409 `{"success":false,"error":"nothing is waiting"}` when none; 400 for another action. Responses that changed the cloud/import routes keep their existing fields and add `"held": true|false`.
  - `layout_json`'s `pending` = `{"source","changes":N}` from the slot.
  - fixtures: add `'/api/layout/pending': { pending: null }`.

- [ ] **Step 1: Extend the e2e** (after Task 2's checks; uses `linux/tools/mock_cca_http.py` exactly as `cca-e2e.sh` does, set through `POST /api/config` `cca_ip`):
  - not manual (fresh state dir section): a CCA sync labels the nodes as today; `/api/layout` `pending` null.
  - manual: after an editor Save, a CCA sync whose layout differs → node labels unchanged, `/api/layout` `pending.source` "cca", `GET /api/layout/pending` lists the moved panels.
  - keep: `POST {"action":"keep"}` → `pending` null; sync again with the same mock data → still null; restart daemon, sync again → still null.
  - a different CCA layout after keep → held again.
  - use: `POST {"action":"use"}` → nodes carry the CCA labels, `manual` false.
  - identical incoming while manual → `pending` null.
  - node import while manual with different labels → nodes' labels unchanged, `pending.source` "import".
  - `POST /api/layout/pending {"action":"use"}` with nothing waiting → 409.

- [ ] **Step 2: Run to verify it fails** — same command as Task 2 Step 2. Expected: FAIL at "manual: … node labels unchanged".

- [ ] **Step 3: Implement** the interfaces above.

- [ ] **Step 4: Run to verify it passes** — `layout-e2e.sh`, `cca-e2e.sh`, `e2e.sh` all pass; site tests pass.

- [ ] **Step 5: Commit** — `feat(linux): a sync never overwrites a hand-made layout, it asks`.

---

### Task 4: ESP32 — store, gate and serve the layout

**Files:**
- Modify: `components/tigo_monitor/tigo_monitor.h`, `components/tigo_monitor/tigo_monitor.cpp`
- Modify: `components/tigo_monitor/tigo_cloud.cpp`
- Modify: `components/tigo_server/tigo_web_server.h`, `components/tigo_server/tigo_web_server.cpp`

**Interfaces:**
- Consumes: Task 1 (all of it). JSON shapes identical to Tasks 2–3 (the page is shared).
- Produces on `TigoMonitorComponent` (all under `state_mutex_`):
  - `layout::Layout layout_tree() const` (skeleton ∪ nodes, as Task 2's tree rule) and the `unplaced`/`unassigned` lists the handler needs.
  - `std::string set_layout(const layout::Layout &)` — validate (known barcode = a node with that barcode), write node labels by barcode (`clear_unlisted`), replace `inverters_` from the skeleton (display names via the existing `inv_dn:` NVS keys), string display labels/ratings via the existing setters' storage, `manual_ = true`, save skeleton + node table once, `rebuild_string_groups()`. Returns "" or the error.
  - `layout::Offer offer_layout(const char *source, std::vector<layout::Assignment> incoming, layout::InverterMppts inverters)`; `match_cca_to_uart`, the cloud import in `tigo_cloud.cpp`, and the web server's node import collect assignments and call it instead of writing labels directly (not-manual path behaves exactly as today, including `save_node_table()` + `rebuild_string_groups()` only when something matched).
  - `pending_` (in PSRAM containers), `bool resolve_pending(bool use)`, and the pending JSON for the handler.
  - Skeleton in NVS: `encode_skeleton` split over up to 8 `char[256]` prefs keyed `fnv1_hash("layout_<i>")`; loaded in `setup()` after `load_node_table()`. If a skeleton with `manual` is present, it replaces the YAML `inverters_`; YAML inverters are kept in `yaml_inverters_`; if they differ from the skeleton's, `offer_layout("yaml", {}, yaml list)` holds them. "Use theirs" for source `yaml` restores `yaml_inverters_` and clears `manual`.
- Produces on `TigoWebServer`: handlers for `GET/POST /api/layout` and `GET/POST /api/layout/pending` (4 more URIs → 55 of 60), `check_api_auth` first, bodies read into PSRAM as `api_nodes_import_handler` does (cap 32 KB), responses built in `PSRAMString`, cJSON for parsing.

- [ ] **Step 1: Compile check fails first** — add the four `httpd_uri_t` registrations pointing at not-yet-written handler names.
  Run: `esphome compile boards/test-local-tigomonitor.yaml` (as `.github/workflows/esp32.yml` does).
  Expected: FAIL, undefined handlers.

- [ ] **Step 2: Implement** the component and handler changes above. The host tests from Task 1 cover the rules; this task's own logic is plumbing.

- [ ] **Step 3: Compile both CI configs** — `test-local-tigomonitor` and the relay-standby rewrite from `esp32.yml`. Expected: both succeed; report flash bytes before/after.

- [ ] **Step 4: On-device check (ask the user before flashing; the test AtomS3R on `/dev/ttyACM0`, never `/dev/ttyACM1`)** — flash, then from the host with `curl`: import 4 fake nodes, run the same sequence as Task 2's e2e (save, rename, reject duplicate, reboot via `/api/restart` and re-read) and Task 3's keep/use against a cloud-import-free path (node import while manual → held; keep; use). Record pass/fail per check.

- [ ] **Step 5: Commit** — `feat(tigo_server): lay out the topology by hand on the ESP32 too`, body with compile sizes and the on-device results (or "not flashed" if the user declined).

---

### Task 5: Page — edit mode, tree editing, drag and drop

**Files:**
- Modify: `components/tigo_server/web/app.html` (Topology view markup, CSS, a new `// ===== Topology editor =====` script section)
- Create: `site/test/topology-editor.test.mjs`

**Interfaces:**
- Consumes: `GET/POST /api/layout` (Task 2 shape).
- Produces in `app.html`: `topoEditor = { active, tree, dirty }`; `enterTopoEdit()`, `exitTopoEdit(save: boolean)`; toolbar buttons `#topo-edit-btn` ("Edit layout"), `#topo-save-btn` ("Save layout"), `#topo-cancel-btn` ("Cancel"); tray `#topo-unassigned`; panel cards in edit mode carry `data-barcode` and `draggable="true"`; strings `data-string-label`; add buttons `.topo-add-inverter`, `.topo-add-mppt`, `.topo-add-string`; delete buttons `.topo-del` (shown only on empty items).

Behaviour (from the spec): normal view unchanged; `refreshTopology` keeps live readings flowing into cards in edit mode but never rebuilds the edited tree; entering edit mode puts each `unplaced_mppts` entry into a new inverter "Inverter n"; Cancel restores the last fetched tree; leaving the view or `beforeunload` with `dirty` asks; Save POSTs the tree (panels as barcodes) and on 400 shows the server's `error` without leaving edit mode. Drag: HTML5 drag events for mouse plus pointer events for touch (long-press 300 ms to pick up, so a scroll that starts on a card scrolls). Panel names in the editor are computed client-side with the same rule as `panel_name` so the user sees "A3" before saving.

- [ ] **Step 1: Write the failing browser test** `site/test/topology-editor.test.mjs` (Node test runner + Playwright, serving `app.html` with `page.route` answers from `screenshots/fixtures.mjs` plus an in-memory `/api/layout` that records POST bodies):
  - `Edit layout shows the tray and add buttons; normal view has neither`.
  - `live readings keep updating on cards in edit mode without undoing a drag` (change the `/api/devices` fixture between refreshes).
  - `an empty string, MPPT or inverter can be deleted; one with panels has no delete button`.
  - `leaving the view with unsaved edits asks first` (dialog handler asserts the prompt; dismissing keeps edit mode).
  - `dragging a tray panel into string A appends it and names it A<n>` (mouse `dragTo`).
  - `Save posts the tree with panels as barcodes in order` — assert exact POST body.
  - `Cancel discards a drag` — no POST; tree equals the fetched one.
  - `+ String adds the next free letter; + MPPT the next MPPT n; + Inverter "Inverter n"`.
  - `a 400 from Save shows its error and stays in edit mode`.
  - `touch: long-press then move drops the panel; a quick swipe on a card does not` (context with `hasTouch: true`, dispatched pointer events).
  - `outside edit mode no card is draggable`.

- [ ] **Step 2: Run to verify it fails** — `cd site && node --test test/topology-editor.test.mjs`. Expected: FAIL, `#topo-edit-btn` not found.

- [ ] **Step 3: Implement** in `app.html`. Run `node --check` on the extracted inline script (as done for earlier UI changes).

- [ ] **Step 4: Run to verify it passes** — the new test, then `node --test test/*.test.mjs` (all site tests), then `npm run screenshots` and look at the Topology PNGs (normal view must be unchanged); commit any changed PNGs.

- [ ] **Step 5: Commit** — `feat(web): lay out the topology by dragging panels into strings`.

---

### Task 6: Page — the "differs from yours" banner

**Files:**
- Modify: `components/tigo_server/web/app.html`
- Modify: `site/test/topology-editor.test.mjs`

**Interfaces:**
- Consumes: `layout.pending`, `GET/POST /api/layout/pending` (Task 3 shape).
- Produces: `#topo-pending` banner on the Topology view: "The {CCA | Tigo cloud | imported file | YAML}'s layout differs from yours — Review"; Review expands a list of changes ("A3 → String B / MPPT 2 as B1"), plus inverter changes when present; buttons `Keep mine` / `Use theirs`. The banner also shows in edit mode; there, Use theirs is disabled while there are unsaved edits, with a title saying to save or cancel first.

- [ ] **Step 1: Failing tests** added to the browser test: banner hidden when `pending` null; shown with the source wording for each of the four sources; Review lists the changes from the pending fixture; Keep mine POSTs `{"action":"keep"}` and hides it; Use theirs POSTs `{"action":"use"}` and re-renders; Use theirs disabled while `dirty`.
- [ ] **Step 2: Run, expect FAIL** (`#topo-pending` missing).
- [ ] **Step 3: Implement.** `node --check`.
- [ ] **Step 4: Run all site tests, expect pass.**
- [ ] **Step 5: Commit** — `feat(web): ask before a sync replaces a hand-made layout`.

---

### Task 7: Docs

**Files:**
- Modify: `site/src/content/docs/guides/web-server.mdx` (the Topology section)
- Modify: `site/src/content/docs/guides/raspberry-pi.md` (the "no YAML" gap is closed; point to the editor instead of JSON editing)

Use the `writing-tigo-docs` skill. Mechanism, then what to do: Edit layout → drag → Save; names follow position; what happens when a sync disagrees and what Keep/Use do; limits (32-byte names, ESP32 skeleton 2 KB). No CHANGELOG edit (deferred).

- [ ] **Step 1:** Write the sections. **Step 2:** `cd site && npx astro build` (link validator must pass; screenshots need `npm run screenshots` first). **Step 3:** Commit — `docs: lay out the topology by hand`.

---

### Task 8: Whole-branch verification

- [ ] Full Pi suite in the dev container in the background (`build.sh`, `e2e.sh`, `cca-e2e.sh`, `layout-e2e.sh`, `bridge-e2e.sh`, `master-e2e.sh`, `standby-e2e.sh`, `https-e2e.sh`, `watchdog-e2e.sh`), each exit code recorded.
- [ ] Both ESP32 compiles; site tests; `npx astro build`.
- [ ] Ask the user before installing on the Zero (it is the sole bus master; ~2 s bus gap) or redeploying the Pi5 container. On the live Pi after install: `GET /api/layout` matches the existing CCA-derived layout, `manual` false, nothing pending.
