// site/wizard.js
import { BOARDS, getBoard } from './boards.js';
import { assembleConfig } from './rules.js';
import { toYaml, toSecretsYaml } from './yaml.js';
import { BRIDGE_BOARDS, getBridgeBoard, assembleBridge, toBridgeYaml } from './bridge.js';
import { generateApiKey } from './lib/apikey.mjs';

const $ = (id) => document.getElementById(id);
const boardSel = $('board');
// A monitor decodes the bus itself; a network bridge (bridge.js) only passes
// it to TigoMonitor elsewhere. Each has its own board list and generator.
const isBridge = () => $('build-bridge').checked;
const currentBoard = () => (isBridge() ? getBridgeBoard : getBoard)(boardSel.value);
function fillBoards() {
  boardSel.replaceChildren(...(isBridge() ? BRIDGE_BOARDS : BOARDS).map((b) => {
    const o = document.createElement('option');
    o.value = b.id; o.textContent = b.label;
    return o;
  }));
}
if (new URLSearchParams(location.search).get('build') === 'bridge') $('build-bridge').checked = true;
if (!$('api-key').value) $('api-key').value = generateApiKey();

function readForm() {
  const board = currentBoard();
  return {
    board,
    form: {
      name: $('name').value.trim(),
      numberOfDevices: Number($('num-devices').value) || board.numberOfDevices || 0,
      updateInterval: $('update-interval').value.trim(),
      uart: { tx_pin: $('uart-tx').value.trim(), rx_pin: $('uart-rx').value.trim() },
      wifi: { ssid: $('wifi-ssid').value.trim() || 'YOUR_WIFI_SSID',
              password: $('wifi-pass').value.trim() || 'YOUR_WIFI_PASSWORD',
              staticIp: $('static-ip').value.trim() },
      useSecrets: $('use-secrets').checked,
      apiKey: $('api-key').value.trim(),
      otaPassword: $('ota-pass').value.trim() || 'changeme',
      cca: $('cca').value,
      ccaIp: $('cca-ip').value.trim(),
      ccaMac: $('cca-mac').value.trim(),
      cloudImport: $('cloud').checked,
      display: $('display').checked,
      displayPanel: $('display-panel').value,
    },
  };
}

function syncBoardConstraints(board) {
  if (!board) return;
  // prefill uart, toggle ble/display availability
  $('uart-tx').value = board.uartDefault.tx_pin;
  $('uart-rx').value = board.uartDefault.rx_pin;
  $('num-devices').value = board.numberOfDevices;
  const bleOpt = $('cca').querySelector('option[value=ble]');
  bleOpt.disabled = !board.supports.ble;
  if (!board.supports.ble && $('cca').value === 'ble') $('cca').value = 'none';
  $('display-row').style.display = board.supports.display ? '' : 'none';

  // Boards without PSRAM cannot run tigo_server, and every control in the CCA
  // card is a tigo_server feature. Reset the values as well as hiding the rows:
  // switching boards must not leave a stale selection that rules.js then has to
  // override silently.
  const webServer = board.supportsWebServer !== false;
  if (!webServer) { $('cca').value = 'none'; $('cloud').checked = false; }
  for (const id of ['cca-intro', 'cca-row', 'cloud-row']) {
    $(id).style.display = webServer ? '' : 'none';
  }
  $('no-server-note').style.display = webServer ? 'none' : '';

  // Wired boards have no radio. Hide the WiFi fields rather than generating a
  // config that quietly ignores them; static addressing on these goes under
  // `ethernet: manual_ip:`, which the generated YAML carries as a comment.
  const wired = Boolean(board.ethernet);
  for (const id of ['wifi-ssid-row', 'wifi-pass-row', 'static-ip-row']) {
    $(id).style.display = wired ? 'none' : '';
  }
  $('ethernet-note').style.display = wired ? '' : 'none';

  $('board-notes').textContent = board.notes.join(' ');
}

// A bridge has no UART pins to choose (it has no tx_pin, and its rx pin is the
// board's), no CCA and no extras.
function syncMode() {
  const bridge = isBridge();
  $('monitor-fields').style.display = bridge ? 'none' : '';
  $('cca-card').style.display = bridge ? 'none' : '';
  $('bridge-intro').style.display = bridge ? '' : 'none';
  $('device-legend').textContent = bridge ? 'Device' : 'Device & UART';
  $('static-ip-hint').textContent = bridge
    ? 'Leave blank and your router picks one. A fixed address means TigoMonitor always finds the bridge.'
    : 'Leave blank and your router picks one. Fill it in if you want the dashboard always at the same address.';
  const name = $('name');
  if (bridge && ['', 'tigo-server', 'tigo-monitor'].includes(name.value.trim())) name.value = 'tigo-bridge';
  if (!bridge && name.value.trim() === 'tigo-bridge') name.value = 'tigo-server';
  // So a copied link opens the same build.
  const search = bridge ? '?build=bridge' : '';
  if (location.search !== search) history.replaceState(null, '', location.pathname + search + location.hash);
  fillBoards();
  syncBoard();
}

function syncBoard() {
  const board = currentBoard();
  if (!isBridge()) { syncBoardConstraints(board); return; }
  const wired = Boolean(board.ethernet);
  for (const id of ['wifi-ssid-row', 'wifi-pass-row', 'static-ip-row']) $(id).style.display = wired ? 'none' : '';
  $('ethernet-note').style.display = wired ? '' : 'none';
  $('board-notes').textContent = board.notes.join(' ');
}

let lastYaml = '', lastSecrets = null;
function render() {
  const { board, form } = readForm();
  const bridge = isBridge();
  $('out-name').textContent = `${boardSel.value}.yaml`;
  $('cca-ip-row').style.display = form.cca === 'http' ? '' : 'none';
  $('cca-mac-row').style.display = form.cca === 'ble' ? '' : 'none';
  $('display-panel-row').style.display = !bridge && form.display && board.supports.display ? '' : 'none';
  try {
    const cfg = bridge ? assembleBridge(board, form) : assembleConfig(board, form);
    lastYaml = bridge ? toBridgeYaml(cfg) : toYaml(cfg);
    lastSecrets = toSecretsYaml(cfg);
    $('out').textContent = lastYaml;
    $('warn').textContent = '';
    $('dl-secrets').style.display = lastSecrets ? '' : 'none';
  } catch (e) {
    lastYaml = '';
    lastSecrets = null;
    $('dl-secrets').style.display = 'none';
    $('warn').textContent = e.message;
  }
}

function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/yaml' }));
  a.download = name; a.click(); URL.revokeObjectURL(a.href);
}

boardSel.addEventListener('change', () => { syncBoard(); render(); });
// The build choice swaps the board list, so it must be synced before render
// reads the board.
$('wizard').addEventListener('input', (e) => { if (e.target.name === 'build') syncMode(); render(); });
$('gen-key').addEventListener('click', () => { $('api-key').value = generateApiKey(); render(); });
$('copy').addEventListener('click', () =>
  navigator.clipboard.writeText(lastYaml).catch(() => {
    $('warn').textContent = 'Copy failed — select the text and copy manually.';
  }));
$('dl-yaml').addEventListener('click', () => download(`${boardSel.value}.yaml`, lastYaml));
$('dl-secrets').addEventListener('click', () => { if (lastSecrets) download('secrets.yaml', lastSecrets); });

syncMode();
render();
