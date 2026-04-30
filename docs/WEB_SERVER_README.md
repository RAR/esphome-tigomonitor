# Web Server & API

Tigo Monitor ships a single-page web app and a JSON API on the configured `tigo_server` port (default 80). Everything lives at `/app` — the legacy per-page paths still resolve as 302 redirects so existing bookmarks keep working.

## Single-page app

| View | URL | Notes |
|------|-----|-------|
| Dashboard | `/app#dashboard` | Hero strip · per-string panel heatmap · color legend · alerts |
| History | `/app#history` | TSDB-backed power/energy charts; range tabs (day / week / month / year) |
| Topology | `/app#topology` | Inverter → string → panel tree with live V/I/W/°C; rename + nameplate editing |
| Node Table | `/app#nodes` | Device registry with CCA metadata; Export / Import as JSON |
| Tools | `/app#tools` | YAML generator + Reset Peak / Clear Node Table / Restart |
| Diagnostics | `/app#diagnostics` | Memory · network · UART telemetry · TSDB stats |
| CCA Info | `/app#cca` | Live CCA mirror; manual Refresh button |

`/`, `/nodes`, `/status`, `/yaml`, `/cca`, `/history` all 302 → `/app#<view>`. The redirects use a relative `Location` so they work standalone *and* under HA Ingress without any extra configuration.

### Sidebar

The sidebar's footer carries:

- **GitHub link** → opens `github.com/RAR/esphome-tigomonitor` in a new tab.
- **°C / °F toggle** — persists via `localStorage['tempUnit']` (same key the legacy page used). The button shows the unit you'll *get* if you click. Active view re-renders immediately.
- **Theme toggle** — light/dark, persists via `localStorage['tigoTheme']`.

### Naming overrides

Inverters and strings can be renamed live from the Topology view (✎ next to each label). The override is stored in NVS via ESPHome's `global_preferences` API and is keyed by canonical name (the YAML inverter name or CCA string label). YAML stays the source of truth for identity — overrides are purely display labels and are used wherever those names appear: Topology, Dashboard inverter cards, Dashboard alert text, embedded strings inside `/api/inverters`.

Empty override = falls back to canonical.

### Per-string panel nameplate

Each string can carry a per-panel nameplate watts value (uint16, 0 = unset). Set via the click-to-edit pill in Topology. When set:

- Topology and Dashboard tiles display "350 W (88%)" alongside watts.
- String roll-ups display "Y% of Z kW" (output vs total nameplate).
- `panelClass` switches to rating-vs-power health classification (`<30%` bad, `<70%` warn, else good) with a "string sleeping" check that flips the whole row to dead when total output is `<5%` of total nameplate (so dawn doesn't paint everything red).

Falls back to median-vs-peer behavior when no rating is set.

### Heatmap

The dashboard uses fixed-size colored tiles per panel grouped by string. Color buckets match the legend strip (good ≥70% of reference, warn ≥30%, bad else, dead = string sleeping or telemetry stale). Each tile shows panel name + watts; hover scales the tile and reveals a tooltip with the full reading and "% of rated" if available.

---

## API endpoints

All endpoints return JSON unless noted. Optional Bearer-token auth (see Authentication below).

### Read

| Endpoint | Returns |
|----------|---------|
| `/api/health` | `{status, uptime, heap_free, heap_min_free}` — no auth |
| `/api/status` | ESP32 status + UART counters + RSSI + memory |
| `/api/overview` | System aggregates (`total_power`, `total_energy_in`, `active_devices`, …) |
| `/api/devices` | Per-device live telemetry (`power_in`, `voltage_in`, `current`, `temperature`, `data_age_ms`, …) |
| `/api/strings` | Flat per-string aggregates incl. `display_label`, `panel_rating_w` |
| `/api/inverters` | Hierarchical inverter rollups with embedded strings (each carries `display_label`, `panel_rating_w`) and inverter `display_name` |
| `/api/nodes` | Node table with CCA metadata |
| `/api/cca` | CCA connection state + `device_info` (encoded JSON string from CCA) |
| `/api/yaml?sensors=…&hub_sensors=…` | Generated YAML config (Tools view) |
| `/api/tsdb/stats` | LittleFS partition + per-DB record counts (only when esp_tsdb is compiled in) |
| `/api/history/power?range=day\|week\|month\|year` | System power/energy time series |
| `/api/history/panel?slot=N&range=…` | Single-panel power time series |
| `/api/panels` | Slot map (`barcode → slot`) |
| `/api/energy/history` | Daily energy history (RAM ring buffer, kept alongside TSDB) |

### Write

| Endpoint | Payload | Behaviour |
|----------|---------|-----------|
| `POST /api/restart` | none | Calls `App.safe_reboot()` after sending response |
| `POST /api/reset_peak_power` | none | Clears per-device peak-power high-water marks |
| `POST /api/reset_node_table` | none | Drops the persisted node table; devices repopulate from telemetry |
| `POST /api/nodes/import` | JSON `{nodes:[…]}` | Replaces the node table from a previous Export |
| `POST /api/inverters/rename` | `{name, display_name}` | Set inverter display name. Empty `display_name` clears the override |
| `POST /api/strings/rename` | `{label, display_label}` | Set string display name. Empty clears the override |
| `POST /api/strings/rating` | `{label, rating_w}` | Set per-panel nameplate watts. `rating_w=0` clears |
| `POST /api/cca/refresh` | none | Triggers a fresh CCA query |
| `POST /api/backlight` | `state=on\|off\|toggle` | Backlight control (units with backlight wired) |

### Example

```bash
curl http://192.168.1.100/api/overview
```

```json
{
  "total_power": 4523.5,
  "total_current": 12.3,
  "total_energy_in": 45.6,
  "active_devices": 20,
  "max_devices": 24,
  "avg_efficiency": 96.2,
  "avg_temperature": 42.5
}
```

Renaming an inverter:

```bash
curl -X POST http://192.168.1.100/api/inverters/rename \
  -H 'Content-Type: application/json' \
  -d '{"name":"FlexBoss A","display_name":"South Roof"}'
```

Setting per-panel nameplate watts on a string:

```bash
curl -X POST http://192.168.1.100/api/strings/rating \
  -H 'Content-Type: application/json' \
  -d '{"label":"String A","rating_w":400}'
```

---

## Authentication

### API (Bearer token)

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  api_token: "your-secret-token"
```

```bash
curl -H "Authorization: Bearer your-secret-token" http://esp32/api/devices
```

### Web (HTTP Basic)

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  web_username: "admin"
  web_password: "secure-password"
```

Browser prompts for credentials. Cached per session.

`/api/health` is the only endpoint that ignores both auth schemes.

---

## Home Assistant Ingress

The SPA detects ingress prefixes from `window.location.pathname` and prepends the detected `BASE_PATH` to every `apiFetch`. Legacy-page redirects use a relative `Location` so they resolve under any URL prefix.

To allow the longer URIs HA Ingress generates, add:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_HTTPD_MAX_REQ_HDR_LEN: "2048"
      CONFIG_HTTPD_MAX_URI_LEN: "1024"
```

Compatible with the [hass_ingress](https://github.com/lovelylain/hass_ingress) integration; native HA add-on Ingress also works.

---

## Configuration

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
  api_token: "optional-token"
  web_username: "optional-user"
  web_password: "optional-pass"
  backlight: backlight_id   # optional — enables /api/backlight
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tigo_monitor_id` | ID | required | Reference to `tigo_monitor` |
| `port` | Integer | 80 | HTTP port |
| `api_token` | String | none | Bearer token for `/api/*` |
| `web_username` | String | none | HTTP Basic Auth user |
| `web_password` | String | none | HTTP Basic Auth pass |
| `backlight` | ID | none | Light component for the optional `/api/backlight` endpoint |

---

## Technical notes

- **Framework**: ESP-IDF native `esp_http_server` on a dedicated 8 KB-stack task.
- **Max URI handlers**: 35 routes registered (raise via `config.max_uri_handlers` in `tigo_web_server.cpp` if adding more).
- **Connection model**: 4 max open sockets, keep-alive disabled, LRU purge enabled — minimizes internal RAM footprint without much real-world impact at typical poll rates.
- **HTML assets**: served from `R""` raw-string constants in `web_assets.h`, regenerated from `components/tigo_server/web/*.html` by the Python codegen step. The API token placeholder (`__TIGO_API_TOKEN__`) is substituted at runtime so each device's token stays unique without a recompile.
- **Memory**: response building and HTML buffers go through `PSRAMString` so large pages don't pressure internal heap.
- **Persistence**: NVS (via `global_preferences`) holds inverter/string display-name overrides and panel nameplate ratings. Node table and energy history live in NVS too. TSDB time-series data lives on a separate LittleFS partition — see [`docs/tsdb-integration.md`](tsdb-integration.md).

---

## Browser support

Modern Chrome / Firefox / Safari, mobile or desktop. No plugins. The SPA degrades gracefully if the API token is set incorrectly (visible "refresh error" in topbar).
