// site/wizard.js
import { BOARDS, getBoard } from './boards.js';
import { assembleConfig } from './rules.js';
import { toYaml, toSecretsYaml } from './yaml.js';
import { generateApiKey } from './lib/apikey.mjs';

const $ = (id) => document.getElementById(id);
const boardSel = $('board');
for (const b of BOARDS) {
  const o = document.createElement('option');
  o.value = b.id; o.textContent = b.label; boardSel.appendChild(o);
}
if (!$('api-key').value) $('api-key').value = generateApiKey();

function readForm() {
  const board = getBoard(boardSel.value);
  return {
    board,
    form: {
      name: $('name').value.trim(),
      numberOfDevices: Number($('num-devices').value) || board.numberOfDevices,
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
      cloudImport: $('cloud').checked,
      display: $('display').checked,
    },
  };
}

function syncBoardConstraints(board) {
  // prefill uart, toggle ble/display availability
  if (!$('uart-tx').value) $('uart-tx').value = board.uartDefault.tx_pin;
  if (!$('uart-rx').value) $('uart-rx').value = board.uartDefault.rx_pin;
  if (!$('num-devices').value) $('num-devices').value = board.numberOfDevices;
  const bleOpt = $('cca').querySelector('option[value=ble]');
  bleOpt.disabled = !board.supports.ble;
  if (!board.supports.ble && $('cca').value === 'ble') $('cca').value = 'none';
  $('display-row').style.display = board.supports.display ? '' : 'none';
  $('board-notes').textContent = board.notes.join(' ');
}

let lastYaml = '', lastSecrets = null;
function render() {
  const { board, form } = readForm();
  $('cca-ip-row').style.display = form.cca === 'http' ? '' : 'none';
  try {
    const cfg = assembleConfig(board, form);
    lastYaml = toYaml(cfg);
    lastSecrets = toSecretsYaml(cfg);
    $('out').textContent = lastYaml;
    $('warn').textContent = '';
    $('dl-secrets').style.display = lastSecrets ? '' : 'none';
  } catch (e) {
    $('warn').textContent = e.message;
  }
}

function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/yaml' }));
  a.download = name; a.click(); URL.revokeObjectURL(a.href);
}

boardSel.addEventListener('change', () => { syncBoardConstraints(getBoard(boardSel.value)); render(); });
$('wizard').addEventListener('input', render);
$('gen-key').addEventListener('click', () => { $('api-key').value = generateApiKey(); render(); });
$('copy').addEventListener('click', () => navigator.clipboard.writeText(lastYaml));
$('dl-yaml').addEventListener('click', () => download(`${boardSel.value}.yaml`, lastYaml));
$('dl-secrets').addEventListener('click', () => { if (lastSecrets) download('secrets.yaml', lastSecrets); });

syncBoardConstraints(getBoard(boardSel.value));
render();
