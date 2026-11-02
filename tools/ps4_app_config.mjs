// Rewrites render.width/render.height in a staged app.json so the Tailwind
// classes are compiled for the PS4 surface instead of the app render target.
import fs from 'node:fs';

const [, , configPath, width, height] = process.argv;
if (!configPath || !width || !height) {
  console.error('usage: ps4_app_config.mjs <app.json> <width> <height>');
  process.exit(2);
}
const app = JSON.parse(fs.readFileSync(configPath, 'utf8'));
app.render = {width: Number(width), height: Number(height)};
fs.writeFileSync(configPath, `${JSON.stringify(app, null, 2)}\n`);
