// After the first bundle pass, add every fontSize the bundle actually uses to
// the staged assets.config.js and write it back. The PS4 layout scale differs
// from the 2560x1440 render target, so sizes that were fine at render 2560 (27px, 60px) may not
// be in the app's baked list; rerunning the bundler then bakes exactly those.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const [, , stageDir, bundlePath] = process.argv;
if (!stageDir || !bundlePath) {
  console.error('usage: ps4_bundle_assets.mjs <stage-dir> <app.bundle.js>');
  process.exit(2);
}

const configPath = path.join(stageDir, 'assets.config.js');
let config = {};
if (fs.existsSync(configPath)) {
  config = (await import(`${pathToFileURL(configPath).href}?t=${Date.now()}`)).default ?? {};
}

const source = fs.readFileSync(bundlePath, 'utf8');
const used = new Set();
for (const match of source.matchAll(/\bfontSize\s*:\s*(\d+(?:\.\d+)?)/g)) {
  const size = Math.round(Number(match[1]));
  if (Number.isFinite(size) && size > 0) used.add(size);
}

config.fonts ??= {};
for (const font of Object.values(config.fonts)) {
  if (!font || !Array.isArray(font.sizes)) continue;
  font.sizes = [...new Set([...font.sizes, ...used])].sort((a, b) => a - b);
}

fs.writeFileSync(configPath, `export default ${JSON.stringify(config, null, 2)};\n`);
console.log(`PS4 assets: font sizes ${[...used].sort((a, b) => a - b).join(', ') || '(none)'}`);
