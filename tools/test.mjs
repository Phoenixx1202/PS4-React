#!/usr/bin/env node
// Cross-platform runner for the Node test suites (no shell globs).
import {spawnSync} from 'node:child_process';
import {readdirSync, statSync} from 'node:fs';
import {join, relative, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(fileURLToPath(import.meta.url), '../..');
const scope = process.argv[2] ?? 'all';

function collect(dir, out) {
  for (const entry of readdirSync(dir, {withFileTypes: true})) {
    if (entry.name === 'node_modules' || entry.name.startsWith('.')) continue;
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      collect(path, out);
    } else if (entry.name.endsWith('.test.mjs')) {
      out.push(path);
    }
  }
  return out;
}

const roots = {
  runtime: [join(root, 'runtime', 'js')],
  tailwind: [join(root, 'tools', 'tailwind')],
  all: [join(root, 'runtime', 'js'), join(root, 'tools', 'tailwind')],
}[scope];

if (!roots) {
  console.error(`unknown test scope "${scope}"; use runtime, tailwind or all`);
  process.exit(2);
}

const files = [...new Set(roots.flatMap(dir => collect(dir, [])))].sort();
if (files.length === 0) {
  console.error('no test files found');
  process.exit(2);
}

console.log(`Running ${files.length} test files:\n${files.map(file => `  ${relative(root, file)}`).join('\n')}`);
const result = spawnSync(process.execPath, ['--test', ...files], {stdio: 'inherit', cwd: root});
process.exit(result.status ?? 1);
