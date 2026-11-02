#!/usr/bin/env node
// Dev loop for PS4 React: rebuild the staged app on save and send the JS
// bundle to a console running a build with -DPS4_REACT_DEV_SERVER=ON.
//
//   node tools/dev_server.mjs --host 192.168.1.50
//   node tools/dev_server.mjs --host 192.168.1.50 --sample counter
//
// The console listens on TCP/9001, validates the ERHR frame and reloads the
// bundle in the running runtime. Assets are not hot-reloaded; rebuild and
// reinstall the PKG when an imported image, font or sound changes.
import {spawnSync} from 'node:child_process';
import {cpSync, existsSync, mkdirSync, readFileSync, rmSync, watch} from 'node:fs';
import net from 'node:net';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(fileURLToPath(import.meta.url), '../..');
const args = process.argv.slice(2);

function option(name, fallback = '') {
  const index = args.indexOf(name);
  return index === -1 ? fallback : args[index + 1] ?? fallback;
}

const sample = option('--sample');
const host = option('--host');
const port = Number(option('--port', '9001'));
const once = args.includes('--once');
const dryRun = args.includes('--dry-run');

if (!host && !dryRun) {
  console.error('usage: node tools/dev_server.mjs --host <PS4-IP> [--port 9001] [--sample NAME] [--once] [--dry-run]');
  process.exit(2);
}
if (sample && !/^[A-Za-z0-9._-]+$/.test(sample)) {
  console.error(`invalid sample name: ${sample}`);
  process.exit(2);
}
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error(`invalid port: ${port}`);
  process.exit(2);
}

const appDir = sample ? join(root, 'samples', sample) : join(root, 'app');
const er = join(root, '.deps', 'embeddedReact');
const out = join(root, '.build', 'dev');
const stage = join(out, 'app');
if (!existsSync(join(appDir, 'app.json'))) {
  console.error(`app.json not found: ${appDir}`);
  process.exit(2);
}
if (!existsSync(join(er, 'bridges', 'quickjs', 'js', 'node_modules'))) {
  console.error('Embedded React dependencies are missing; run tools/build_ci.sh or tools/build_ps4.ps1 once.');
  process.exit(2);
}

function run(script, ...params) {
  const result = spawnSync(process.execPath, [join(root, 'tools', script), ...params], {
    stdio: 'inherit',
    cwd: root,
  });
  if (result.status !== 0) throw new Error(`${script} failed (${result.status})`);
}

function bundle() {
  rmSync(stage, {recursive: true, force: true});
  mkdirSync(stage, {recursive: true});
  cpSync(appDir, stage, {recursive: true});
  if (sample) {
    const shared = join(root, 'samples', 'assets');
    if (existsSync(shared)) {
      rmSync(join(out, 'assets'), {recursive: true, force: true});
      cpSync(shared, join(out, 'assets'), {recursive: true});
    }
  }
  run('ps4_app_config.mjs', join(stage, 'app.json'), '1920', '1080');
  run('bundle.mjs', er, stage, out);
  run('ps4_bundle_assets.mjs', stage, join(out, 'app.bundle.js'));
  run('bundle.mjs', er, stage, out);
  return readFileSync(join(out, 'app.bundle.js'));
}

const CRC_TABLE = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let value = i;
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
  CRC_TABLE[i] = value >>> 0;
}

function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) value = CRC_TABLE[(value ^ byte) & 0xff] ^ (value >>> 8);
  return (value ^ 0xffffffff) >>> 0;
}

function frame(payload) {
  const header = Buffer.alloc(16);
  header.write('ERHR', 0, 'ascii');
  header.writeUInt8(1, 4); // protocol version
  header.writeUInt8(1, 5); // LOAD_CONTAINER
  header.writeUInt16LE(0, 6);
  header.writeUInt32LE(payload.length, 8);
  header.writeUInt32LE(crc32(payload), 12);
  return Buffer.concat([header, payload]);
}

function send(payload) {
  const bytes = frame(payload);
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (error, ok) => {
      if (settled) return;
      settled = true;
      error ? reject(error) : resolve(ok);
    };
    const socket = net.createConnection({host, port});
    socket.setTimeout(10000);
    socket.on('connect', () => socket.write(bytes));
    socket.on('data', data => {
      if (!data.length) return;
      socket.end();
      finish(null, data[0] === 0);
    });
    socket.on('timeout', () => {
      socket.destroy();
      finish(new Error(`timed out connecting to ${host}:${port}`));
    });
    socket.on('error', error => finish(error));
    socket.on('close', () => finish(new Error('console closed the connection without an ack')));
  });
}

async function push() {
  try {
    const payload = bundle();
    if (dryRun) {
      console.log(`bundle ok: ${payload.length} bytes (dry run)`);
      return;
    }
    const ok = await send(payload);
    console.log(ok
      ? `reload ok: ${payload.length} bytes`
      : 'console rejected the bundle; see /data/PS4React/react.log');
  } catch (error) {
    console.error(error.message);
  }
}

await push();
if (once || dryRun) process.exit(0);

let timer = null;
const schedule = () => {
  clearTimeout(timer);
  timer = setTimeout(push, 200);
};
watch(appDir, {recursive: true}, schedule);
if (sample && existsSync(join(root, 'samples', 'assets')))
  watch(join(root, 'samples', 'assets'), {recursive: true}, schedule);

console.log(`watching ${appDir} -> ${host}:${port} (Ctrl+C to stop)`);
process.on('SIGINT', () => process.exit(0));
