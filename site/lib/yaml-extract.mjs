// site/lib/yaml-extract.mjs
// Targeted, line-based reader for the handful of safety-critical keys the
// drift-guard checks. NOT a general YAML parser — deliberately narrow.
export function extractBoardFields(text) {
  const line = (re) => {
    const m = text.match(re);
    return m ? m[1].trim() : null;
  };
  // NOTE: narrow reader — assumes flash_size/partitions appear only under the
  // esp32: block (true for all ESPHome board configs) and esp32_hosted: is a
  // block-form key, not inline. Sufficient for the board files we check.
  const flash_size = line(/^\s*flash_size:\s*(\S+)/m);
  const partitions = line(/^\s*partitions:\s*(\S+)/m);
  // Capture the top-level `psram:` block: its header line plus all following
  // indented body lines, stopping at the next unindented line or EOF.
  const psramBlock = text.match(/^psram:[^\n]*\n((?:[ \t]+[^\n]*\n?)*)/m);
  const pick = (blk, key) => {
    if (!blk) return null;
    const m = blk.match(new RegExp(`^\\s*${key}:\\s*(\\S+)`, 'm'));
    return m ? m[1].trim() : null;
  };
  const psramMode = pick(psramBlock?.[1], 'mode');
  const psramSpeed = pick(psramBlock?.[1], 'speed');
  // `^\s*` anchors to line start so a commented-out `#   enable_idf_...` line
  // (an opt-in left disabled) is correctly read as NOT enabled.
  const experimental = /^\s*enable_idf_experimental_features:\s*(yes|true)\b/m.test(text);
  const hasHosted = /^esp32_hosted:\s*$/m.test(text);
  const components = [...text.matchAll(/^\s*-\s*(?:name:\s*)?([A-Za-z0-9_./^-]+)\s*$/gm)]
    .map((m) => m[1])
    .filter((c) => c.includes('/'));
  return { flash_size, partitions, psramMode, psramSpeed, experimental, hasHosted, components };
}
