# Design: Tigo Monitor Config Wizard (GitHub Pages)

**Date:** 2026-07-21
**Status:** Approved (design), pending implementation plan
**Topic:** A static, GitHub Pages–hosted wizard that generates a correct starter
ESPHome YAML for the Tigo Monitor, tailored to the user's board.

## Problem

New users hand-assemble their first board config and hit board-specific traps.
The motivating case (discussion #31): an ESP32-P4 config requested `psram:
speed: 200MHz` without `enable_idf_experimental_features: yes`, so PSRAM came up
unstable and the board crash-looped at boot before ESPHome even started. The
board files in `boards/` already encode the *correct* per-board settings, but a
first-time user has no guided way to produce an equivalent config for their
hardware. There is an on-device YAML generator (Tools → YAML), but it emits the
per-panel **sensor** block from live discovery — a post-setup job — not the
day-0 **board scaffolding** where these traps live.

## Goals

- Produce a **complete, ready-to-flash starter config** for a chosen curated
  board: board/framework/PSRAM/partitions scaffolding + UART + `tigo_monitor` +
  `tigo_server` + core system sensors.
- Encode the per-board gotchas so the output is correct by construction (the P4
  experimental-features flag, `esp_hosted` companion radio, flash size,
  partition CSV, PSRAM mode/speed, key sdkconfig).
- Expose the cross-cutting toggles that change scaffolding: **CCA source**
  (none / HTTP / BLE), **Tigo cloud import**, and **secrets scaffolding**
  (Wi-Fi/OTA secrets + in-browser API encryption-key generation).
- Host it for free as a static site on GitHub Pages.
- Keep the wizard's board knowledge from silently drifting out of sync with the
  real `boards/*.yaml`.

## Non-goals (YAGNI)

- **No panel/inverter/MPPT layout builder.** Panels auto-discover on the bus and
  the on-device Tools → YAML generator emits the per-panel/string sensor block;
  re-implementing that in the browser is out of scope.
- **No custom/advanced board builder.** Only curated, known-good boards, so the
  tool can't emit an un-vetted (chip × flash × PSRAM) combination — that would
  reintroduce exactly the misconfiguration risk this tool exists to remove.
- **No backend.** Entirely client-side; nothing the user types (secrets,
  passwords) leaves the browser.
- **No on-device compile in CI.** Too heavy; covered by a manual compile-check
  of a generated config, as the repo already does for its board files.

## Scope decisions (from brainstorming)

| Question | Decision |
| --- | --- |
| Output scope | Complete starter config (scaffolding + core, no panel builder) |
| Board coverage | Curated repo boards only |
| Toggles | CCA (HTTP/BLE), cloud import, secrets scaffolding |
| Architecture | Static wizard, hand-authored board data + CI drift-guard |
| Location | Top-level `site/` |
| CI test | Node-based |

## Architecture

Approach A: a pure static site — no framework, no bundler, no build step. It
runs by opening `index.html` locally and unchanged on GitHub Pages.

```
site/
  index.html      # markup + section scaffolding
  style.css       # dark theme mirroring the on-device dashboard
  boards.js       # curated board data (the per-board source of truth for the wizard)
  rules.js        # cross-cutting toggle transforms (CCA/BLE/cloud)
  yaml.js         # deterministic config-object -> YAML string
  wizard.js       # UI wiring, live preview, copy/download
  boards.test.mjs # CI drift-guard + YAML smoke test (Node, zero-dep)
```

Hosting: `.github/workflows/pages.yml` deploys `site/` via `actions/deploy-pages`
on pushes touching `site/**`. One-time manual step: set the repo's Pages source
to "GitHub Actions". Kept separate from the markdown `docs/` tree so Pages does
not attempt to serve those.

### Data flow

```
board dropdown + form inputs
      -> wizard.js builds an in-memory config object from boards.js[selected]
      -> rules.js applies enabled toggles as transforms over that object
      -> yaml.js serializes the object to YAML (+ optional secrets.yaml)
      -> live preview pane; Copy / Download actions
```

All state lives in the browser tab; there is no persistence and no network I/O.

## Component design

### `boards.js` — curated board data

One object per curated board. Shape:

```js
{
  id: 'esp32s3-atoms3r',
  label: 'M5Stack AtomS3R (ESP32-S3, 8MB)',
  chip: 'esp32s3',                     // esp32s3 | esp32p4 | esp32
  board: 'm5stack-atoms3',             // platformio board id
  variant: 'esp32s3',                  // when needed
  flash_size: '8MB',
  partitions: { default: 'partitions/tigo-8mb.csv',
                ble:     'partitions/tigo-8mb-ble.csv' },
  psram: { mode: 'octal', speed: '80MHz' } | null,
  framework: { advanced: { enable_idf_experimental_features: false } },
  sdkconfig: { CONFIG_UART_ISR_IN_IRAM: 'y', /* vetted per-board */ },
  hosted: null | { variant, slot, clk_pin, cmd_pin, d0_pin, d1_pin,
                   d2_pin, d3_pin, reset_pin, active_high },
  uart_default: { tx_pin: 'GPIO1', rx_pin: 'GPIO2' },
  supports: { ble: true, display: false },
  esp_tsdb: { registry: 'zakery292/esp_tsdb^2.1.0' }   // or fork ref for P4
            | { fork: { source, ref } },
  notes: [ 'string caveats surfaced in the UI' ],
}
```

Curated set (from `boards/`): AtomS3R, AtomS3R+display, AtomS3, ESP32-P4
EVBoard, generic ESP32 devkit. The P4 entry carries `enable_idf_experimental_features:
true`, the `hosted` C6 block, `flash_size: 16MB`, `partitions/tigo-16mb.csv`, the
esp_tsdb fork ref, and the 200 MHz-PSRAM note.

### `rules.js` — toggle transforms

Each enabled toggle is a pure function `(cfg, board) => cfg'`:

- **`cca: 'ble'`** — switch `partitions` to the board's `ble` CSV; add the
  `esp32_ble` / `ble_client` stubs; set `tigo_server.cca_source: ble`; ensure the
  SHA-512 requirement is met. Disabled (with an inline reason) when
  `board.supports.ble` is false.
- **`cca: 'http'`** — set `tigo_server.cca_source: http` and add `cca_ip`.
- **`cloud_import`** — set `tigo_server.cloud_import: true` (pulls in the cert
  bundle + SHA-512 at compile time).

Keeping these here, not in the per-board data, isolates the "configure
appropriately" logic from the board facts.

### `yaml.js` — generation

Deterministic assembly into a fixed block order:

```
esphome -> esp32 -> psram -> esp32_hosted -> wifi -> logger -> api -> ota
  -> uart -> tigo_monitor -> tigo_server
  -> sensor (core system sensors) -> text_sensor: -> binary_sensor:
```

The empty `text_sensor:` / `binary_sensor:` sections are always emitted — the
components fail to compile without them (CLAUDE.md rule). When secrets
scaffolding is on, credential fields emit `!secret` references and a matching
`secrets.yaml` is generated alongside; the API encryption key is generated in
the browser (Web Crypto random 32 bytes → base64). A header comment records the
generator, the chosen board, and a link back to the repo.

### `wizard.js` — UI

Single scrolling page, form on the left, live YAML preview on the right.
Sections: Board, Basics (`name`, `number_of_devices`, `update_interval`, UART
pins prefilled from the board), Connectivity (Wi-Fi SSID/pass, optional static
IP), CCA (none/HTTP/BLE), Cloud import, Secrets (generate-key button, OTA
password). Incompatible options disable themselves with an inline reason.
Actions: Copy, Download `<board>.yaml`, Download `secrets.yaml`.

## Error handling

- Options invalid for the chosen board (e.g. BLE where unsupported) are disabled
  with a visible reason rather than silently dropped.
- Required text inputs left blank fall back to clearly-placeholder values
  (`YOUR_WIFI_SSID`, etc.) so the preview always renders a complete file, with an
  inline "fill these in" hint.
- The generator is total: every reachable config object produces valid YAML; the
  smoke test enforces this.

## Testing & CI drift-guard

`site/boards.test.mjs` (Node, zero external deps), run in the Pages workflow (or
a lightweight CI job):

1. **Drift-guard** — for each board in `boards.js`, read the corresponding
   `boards/*.yaml` and assert the safety-critical fields match: `flash_size`,
   `partitions` (default and BLE), `psram.mode`/`psram.speed`,
   `enable_idf_experimental_features`, `esp_hosted` presence, and a small set of
   key sdkconfig options. Divergence fails the build. This is what lets the
   hand-authored board data stay trustworthy without a fragile YAML→metadata
   build.
2. **YAML smoke test** — generate a config for each board × a couple of toggle
   combinations and assert each parses as valid YAML.

Manual verification (unchanged from repo practice): compile one generated config
end-to-end before relying on it.

## Future extensions (not in this project)

- Custom/advanced board path with explicit guardrails.
- Import an existing config to pre-fill the form.
- Deep-link/query-string presets for support ("open the wizard with the P4
  preset").
