#!/usr/bin/env node
// Generate the C header the native host uses for save-data titles and logs.
import {readFileSync, writeFileSync} from 'node:fs';

const [, , configPath, outputPath] = process.argv;
if (!configPath || !outputPath) {
  console.error('usage: app_header.mjs <app.json> <app_config.generated.h>');
  process.exit(2);
}

const app = JSON.parse(readFileSync(configPath, 'utf8'));
const parts = String(app.version ?? '01.00').split('.');
const version = `${(parts[0] ?? '01').padStart(2, '0').slice(0, 2)}.${(parts[1] ?? '00').padStart(2, '0').slice(0, 2)}`;
const c = value => JSON.stringify(String(value ?? ''));

writeFileSync(outputPath, `#pragma once
// Generated from app.json by tools/app_header.mjs.
#define PS4_REACT_TITLE_ID ${c(app.titleId)}
#define PS4_REACT_CONTENT_ID ${c(app.contentId)}
#define PS4_REACT_APP_NAME ${c(app.name)}
#define PS4_REACT_APP_VERSION ${c(version)}
#define PS4_REACT_PRIVILEGED ${app.privileged ? 1 : 0}
#define PS4_REACT_GPU ${app.gpu ? 1 : 0}
`);
