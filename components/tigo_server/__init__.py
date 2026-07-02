import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import tigo_monitor, light, sensor, ble_client, esp32_ble_tracker
from esphome.const import CONF_ID, CONF_PORT
from pathlib import Path

DEPENDENCIES = ['tigo_monitor']
CODEOWNERS = ['@yourusername']

tigo_server_ns = cg.esphome_ns.namespace('tigo_server')
TigoWebServer = tigo_server_ns.class_('TigoWebServer', cg.Component)

# CCA Info page data source. With cca_source: ble (or auto) the web server talks the
# CCA's mobile_api over Bluetooth itself (TigoWebServer is the BLEClientNode), so the
# page works when the CCA's local HTTP API is locked down (firmware 4.0.4+).
CcaSource = tigo_server_ns.enum('CcaSource', is_class=True)
CCA_SOURCES = {'http': CcaSource.HTTP, 'ble': CcaSource.BLE, 'auto': CcaSource.AUTO}

CONF_TIGO_MONITOR_ID = 'tigo_monitor_id'
CONF_API_TOKEN = 'api_token'
CONF_WEB_USERNAME = 'web_username'
CONF_WEB_PASSWORD = 'web_password'
CONF_BACKLIGHT = 'backlight'
CONF_INTERNAL_TEMPERATURE_ID = 'internal_temperature_id'
CONF_CCA_SOURCE = 'cca_source'
CONF_BLE_CLIENT_ID = 'ble_client_id'  # matches ble_client.register_ble_node's key
CONF_CLOUD_IMPORT = 'cloud_import'


def _validate_cca_source(config):
    if config[CONF_CCA_SOURCE] in ('ble', 'auto'):
        if CONF_BLE_CLIENT_ID not in config:
            raise cv.Invalid(
                f"cca_source: {config[CONF_CCA_SOURCE]} requires a ble_client_id "
                "pointing at a ble_client: configured with the CCA's BLE MAC"
            )
        # Auto-resolve the esp32_ble_tracker so we can register as an advertisement
        # listener at codegen time — that's what defines the LISTENER_COUNT macro and
        # compiles in the parse_device() dispatch (a runtime register_listener() is a
        # no-op without it). Only injected for BLE builds, so HTTP builds need no tracker.
        config[esp32_ble_tracker.CONF_ESP32_BLE_ID] = \
            esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA({})[esp32_ble_tracker.CONF_ESP32_BLE_ID]
    return config


def _final_validate(config):
    # ESPHome disables mbedtls SHA-384/512 on IDF >= 6.0 to save flash. Both of our
    # crypto paths need them back:
    #  * cloud_import: everything above the leaf in Tigo's cert chain is SHA-384-signed
    #    (WE1 is ecdsa-with-SHA384, the pinned GTS roots are SHA-384-self-signed).
    #    Without SHA-384 the OID table loses those algorithms, the roots fail to parse
    #    (-0x2100) and TLS silently drops WE1 from the server's chain (-0x3000).
    #  * CCA BLE: the mobile_api session key is a SHA-512 hash (via PSA on IDF 6.0);
    #    without SHA-512, psa_hash_compute() fails and the key would be garbage.
    # This must run before esp32's to_code() reads the flag, hence final-validate and
    # not our own to_code(). require_mbedtls_sha512() doesn't exist on older ESPHome —
    # but those versions never disable SHA-384/512, so skipping the call is correct.
    if config[CONF_CLOUD_IMPORT] or CONF_BLE_CLIENT_ID in config:
        try:
            from esphome.components.esp32 import require_mbedtls_sha512
            require_mbedtls_sha512()
        except ImportError:
            pass
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(TigoWebServer),
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(tigo_monitor.TigoMonitorComponent),
    cv.Optional(CONF_PORT, default=80): cv.port,
    cv.Optional(CONF_API_TOKEN): cv.string,
    cv.Optional(CONF_WEB_USERNAME): cv.string,
    cv.Optional(CONF_WEB_PASSWORD): cv.string,
    cv.Optional(CONF_BACKLIGHT): cv.use_id(light.LightState),
    # Optional: reference an existing internal_temperature sensor for the
    # Diagnostics die-temp readout instead of installing our own. Avoids the
    # single-peripheral install conflict on the ESP32 (#28).
    cv.Optional(CONF_INTERNAL_TEMPERATURE_ID): cv.use_id(sensor.Sensor),
    # CCA Info page source: 'http' (default, CCA local web API), 'ble' (talk the CCA
    # over Bluetooth — for firmware 4.0.4+ that locks HTTP), or 'auto' (BLE if it has
    # data, else HTTP). 'ble'/'auto' require ble_client_id.
    cv.Optional(CONF_CCA_SOURCE, default='http'): cv.one_of(*CCA_SOURCES, lower=True),
    cv.Optional(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
    # Enable the Tigo cloud layout import (panel names + string/MPPT structure when the
    # CCA's local HTTP API is locked down). Credentials are entered in the web UI.
    cv.Optional(CONF_CLOUD_IMPORT, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA), _validate_cca_source)


# ---------------------------------------------------------------------------
# HTML asset embedding
# ---------------------------------------------------------------------------
# Each web/<page>.html file becomes two C++ raw-string constants in the
# generated `web_assets.h`: <PAGE>_HTML_PRE and <PAGE>_HTML_POST, split at the
# `__TIGO_API_TOKEN__` placeholder. Handlers send PRE + api_token_ + POST so
# the runtime substitution stays exactly as it was before extraction.
#
# Pages without the placeholder get one constant: <PAGE>_HTML.
#
# The header is regenerated on every config — the .html files are the source
# of truth, and the generated header is a build artifact (gitignored).

# Pages retired into the SPA still live as 302 redirects to /app#<view>:
#   - '/'       — see dashboard_handler  (302 → /app#dashboard)
#   - 'history' (R3) — see history_handler
#   - 'nodes'   (R4) — see node_table_handler
#   - 'status'  (R5) — see esp_status_handler (renamed to /app#diagnostics)
#   - 'yaml'    (R6) — see yaml_config_handler (renamed to /app#tools)
#   - 'cca'     (R7) — see cca_info_handler
WEB_PAGES = ['app']
TOKEN_PLACEHOLDER = '__TIGO_API_TOKEN__'


def _pick_raw_delim(content: str) -> str:
    """Find a raw-string delimiter that doesn't appear in `content`."""
    delim = 'TIGO'
    while f'){delim}"' in content:
        delim += 'X'
    return delim


def _generate_web_assets_header():
    """Write web_assets.h alongside tigo_web_server.cpp.

    Reads each web/<page>.html, splits at the API-token placeholder, and emits
    raw C++ string constants. Re-run on every codegen pass so the header
    tracks the .html source files.
    """
    component_dir = Path(__file__).parent
    web_dir = component_dir / 'web'
    out = component_dir / 'web_assets.h'

    lines = [
        '// Auto-generated by tigo_server/__init__.py — do not edit.',
        '// Source: components/tigo_server/web/*.html',
        '#pragma once',
        'namespace esphome {',
        'namespace tigo_server {',
        '',
    ]

    for page in WEB_PAGES:
        path = web_dir / f'{page}.html'
        if not path.exists():
            raise FileNotFoundError(f'expected web asset {path}')
        content = path.read_text()
        const_base = f'{page.upper()}_HTML'
        if TOKEN_PLACEHOLDER in content:
            pre, post = content.split(TOKEN_PLACEHOLDER, 1)
            d_pre = _pick_raw_delim(pre)
            d_post = _pick_raw_delim(post)
            lines.append(
                f'inline constexpr const char {const_base}_PRE[] = '
                f'R"{d_pre}({pre}){d_pre}";'
            )
            lines.append(
                f'inline constexpr const char {const_base}_POST[] = '
                f'R"{d_post}({post}){d_post}";'
            )
        else:
            d = _pick_raw_delim(content)
            lines.append(
                f'inline constexpr const char {const_base}[] = '
                f'R"{d}({content}){d}";'
            )
        lines.append('')

    lines += ['}  // namespace tigo_server', '}  // namespace esphome', '']
    out.write_text('\n'.join(lines))


_generate_web_assets_header()


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Get the tigo_monitor component
    parent = await cg.get_variable(config[CONF_TIGO_MONITOR_ID])
    cg.add(var.set_tigo_monitor(parent))

    # Set the port
    cg.add(var.set_port(config[CONF_PORT]))

    # Set the API token if provided
    if CONF_API_TOKEN in config:
        cg.add(var.set_api_token(config[CONF_API_TOKEN]))

    # Set the web authentication if provided
    if CONF_WEB_USERNAME in config:
        cg.add(var.set_web_username(config[CONF_WEB_USERNAME]))
    if CONF_WEB_PASSWORD in config:
        cg.add(var.set_web_password(config[CONF_WEB_PASSWORD]))

    # Set the backlight if provided
    if CONF_BACKLIGHT in config:
        backlight = await cg.get_variable(config[CONF_BACKLIGHT])
        cg.add(var.set_backlight(backlight))

    # Read die temperature from an existing internal_temperature sensor if given
    if CONF_INTERNAL_TEMPERATURE_ID in config:
        temp_sensor = await cg.get_variable(config[CONF_INTERNAL_TEMPERATURE_ID])
        cg.add(var.set_internal_temperature_sensor(temp_sensor))

    # CCA Info page data source
    cg.add(var.set_cca_source(CCA_SOURCES[config[CONF_CCA_SOURCE]]))

    # When BLE-sourced, register TigoWebServer as a BLE node of the given ble_client
    # and define USE_TIGO_CCA_BLE so the (guarded) BLE client code compiles in. Plain
    # HTTP builds skip all of this — no ble_client dependency, no flash cost.
    if CONF_BLE_CLIENT_ID in config:
        cg.add_define('USE_TIGO_CCA_BLE')
        await ble_client.register_ble_node(var, config)
        # Also register as a tracker advertisement listener so the CCA BLE-search works
        # (bumps ESPHOME_ESP32_BLE_TRACKER_LISTENER_COUNT and wires up parse_device()).
        await esp32_ble_tracker.register_ble_device(var, config)

    # Tigo cloud layout import: compile the (guarded) cloud client + UI. HTTPS to
    # mapi.tigoenergy.com verifies against pinned Google Trust Services roots
    # (tigo_cloud_ca.h, attached via cert_pem in tigo_cloud.cpp).
    # (SHA-384/512 support for the TLS chain / BLE session key is requested in
    # _final_validate above — it has to happen before esp32's to_code runs.)
    if config[CONF_CLOUD_IMPORT]:
        cg.add_define('USE_TIGO_CLOUD')
