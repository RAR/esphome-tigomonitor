// site/lib/yaml-extract.mjs
// Targeted, line-based reader for the handful of safety-critical keys the
// drift-guard checks. NOT a general YAML parser — deliberately narrow.
export function extractBoardFields(text) {
  const line = (re) => {
    const m = text.match(re);
    return m ? m[1].trim() : null;
  };
  const flash_size = line(/^\s*flash_size:\s*(\S+)/m);
  const partitions = line(/^\s*partitions:\s*(\S+)/m);
  // top-level psram: block
  const psramBlock = text.match(/^psram:\s*$([\s\S]*?)(?=^\S|\Z)/m);
  const pick = (blk, key) => {
    if (!blk) return null;
    const m = blk.match(new RegExp(`^\\s*${key}:\\s*(\\S+)`, 'm'));
    return m ? m[1].trim() : null;
  };
  const psramMode = pick(psramBlock?.[1], 'mode');
  const psramSpeed = pick(psramBlock?.[1], 'speed');
  const experimental = /enable_idf_experimental_features:\s*(yes|true)\b/.test(text);
  const hasHosted = /^esp32_hosted:\s*$/m.test(text);
  const components = [...text.matchAll(/^\s*-\s*(?:name:\s*)?([A-Za-z0-9_./^-]+)\s*$/gm)]
    .map((m) => m[1])
    .filter((c) => c.includes('/'));
  return { flash_size, partitions, psramMode, psramSpeed, experimental, hasHosted, components };
}
