// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {acquireImage, Archives, Controller, Dialog, Downloads, FileSystem, Http, Keyboard, Music, Packages, Payloads, Power, SaveData, Sound, Sse, WebSocket} from './native.js';

test('SaveData forwards mount, unmount and remove to the host', () => {
  assert.throws(() => SaveData.mount({dirName: 'slot1'}), /PS4 React host/);
  globalThis.__ps4ReactNative = {};
  assert.throws(() => SaveData.mount({dirName: 'slot1'}), /not available/);
  const calls = [];
  globalThis.__ps4ReactNative = {saveData: {
    mount: (...args) => { calls.push(args); return {path: '/savedata0'}; },
    unmount: dir => { calls.push(['unmount', dir]); return true; },
    remove: dir => { calls.push(['remove', dir]); return true; },
  }};
  try {
    assert.deepEqual(SaveData.mount({dirName: 'slot1', blocks: 128, title: 'Save'}),
      {path: '/savedata0'});
    assert.deepEqual(calls, [['slot1', 128, 'Save', '', '', false]]);
    assert.equal(SaveData.unmount('slot1'), true);
    assert.equal(SaveData.remove('slot1'), true);
    assert.deepEqual(calls.slice(1), [['unmount', 'slot1'], ['remove', 'slot1']]);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('Keyboard.prompt resolves the text and cancels through the task', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, snapshots = [];
  const calls = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {ime: {
    start: (...args) => { calls.push(args); return 1; },
    poll: () => snapshots,
    cancel: () => {},
  }};
  try {
    const prompt = Keyboard.prompt({title: 'Name', value: 'A', maxLength: 32});
    assert.deepEqual(calls, [['Name', '', 'A', 32, 'default']]);
    snapshots = [{id: 1, state: 'completed', text: 'Alice', error: ''}];
    tick();
    assert.equal((await prompt.done).text, 'Alice');
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('native network tasks deliver progress, release listeners and stop polling', async () => {
  let tick;
  let clears = 0;
  let snapshots = [];
  const cancelled = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => { clears++; };
  globalThis.__ps4ReactNative = {network: {
    download: () => 1, request: () => 2,
    cancel: id => cancelled.push(id), poll: () => snapshots, version: () => 'fixture',
  }};
  try {
    const task = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const states = [];
    const unsubscribe = task.subscribe(progress => states.push(progress.state));
    snapshots = [{id: 1, state: 'downloading', body: 'must not reach progress', written: 12}];
    tick();
    assert.equal(task.snapshot.destination, '/download0/file');
    assert.equal('body' in task.snapshot, false);
    unsubscribe();
    snapshots = [{id: 1, state: 'completed', written: 24}];
    tick();
    assert.equal((await task.done).written, 24);
    assert.deepEqual(states, ['queued', 'downloading']);
    assert.equal(clears, 1);
    task.cancel();
    assert.deepEqual(cancelled, []);
    const aborted = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    aborted.cancel();
    snapshots = [{id: 1, state: 'cancelled'}];
    tick();
    await assert.rejects(aborted.done, error => error.name === 'AbortError' && error.message.includes('/download0/file'));
    assert.deepEqual(cancelled, [1]);
    const response = Http.request('https://example.com/json');
    snapshots = [{id: 2, state: 'completed', status: 404, url: 'https://example.com/final', headers: {'content-type': 'application/json'}, body: '{"value":42}'}];
    tick();
    const result = await response;
    assert.equal(result.ok, false);
    assert.equal(result.status, 404);
    assert.equal(result.url, 'https://example.com/final');
    assert.equal(result.headers['content-type'], 'application/json');
    assert.equal(Object.isFrozen(result.headers), true);
    assert.deepEqual(await result.json(), {value: 42});
    assert.equal(await result.text(), '{"value":42}');
    assert.equal(clears, 3);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('removeAsync resolves once the worker removed the path and rejects with the path and system error', async () => {
  let tick;
  let snapshots = [];
  const started = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {
    fs: {removeAsync: path => {
      if (path.includes('..')) throw new Error(`fs.removeAsync ${path}: Path is not allowed`);
      return started.push(path);
    }},
    removals: {poll: () => snapshots},
  };
  try {
    const removed = FileSystem.removeAsync('/download0/game');
    const refused = FileSystem.removeAsync('/download0/locked');
    assert.deepEqual(started, ['/download0/game', '/download0/locked']);
    snapshots = [{id: 1, state: 'removing', error: ''}, {id: 2, state: 'queued', error: ''}];
    tick();
    snapshots = [{id: 1, state: 'completed', error: ''}, {id: 2, state: 'failed', error: 'Permission denied'}];
    tick();
    assert.equal(await removed, undefined);
    await assert.rejects(refused, {message: 'fs.removeAsync /download0/locked: Permission denied'});
    await assert.rejects(FileSystem.removeAsync('/download0/../x'), /Path is not allowed/);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('remote images poll only while loads are pending and release every reference', () => {
  const released = [];
  let finished = [];
  let polls = 0;
  globalThis.__ps4ReactNative = {image: {
    load: (uri, width, height) => (width === 10
      ? {id: 7, state: 'ready', name: '@image:7', width: 10, height: 10, error: ''}
      : {id: 8, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: id => released.push(id),
    poll: () => { polls++; const out = finished; finished = []; return out; },
  }};
  const seen = [];
  const releaseReady = acquireImage('https://example.com/a.jpg', 10, 10, 0, false, result => seen.push(result.state));
  globalThis.__ps4ReactFrame(16);
  assert.equal(polls, 0, 'a cached image needs no polling');
  const releaseLoading = acquireImage('https://example.com/b.jpg', 20, 20, 0, false, result => seen.push(result.state));
  globalThis.__ps4ReactFrame(16);
  finished = [{id: 8, state: 'ready', name: '@image:8', width: 20, height: 20, error: ''}];
  globalThis.__ps4ReactFrame(16);
  globalThis.__ps4ReactFrame(16);
  assert.equal(polls, 2, 'polling stops once nothing is pending');
  assert.deepEqual(seen, ['ready', 'loading', 'ready']);
  releaseReady();
  releaseLoading();
  assert.deepEqual(released, [7, 8]);
  globalThis.__ps4ReactFrame(16);
  globalThis.__ps4ReactFrame(16);
  assert.equal(polls, 3, 'a release polls once more so eviction can run');
  delete globalThis.__ps4ReactNative;
});

test('Power.keepAwake passes a boolean to the host', () => {
  const calls = [];
  globalThis.__ps4ReactNative = {power: {keepAwake: enabled => calls.push(enabled)}};
  try {
    Power.keepAwake(true);
    Power.keepAwake(0);
    assert.deepEqual(calls, [true, false]);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('Sound passes the name, volume and pan to the host and returns whether it started', () => {
  const calls = [];
  globalThis.__ps4ReactNative = {sound: {
    play: (name, volume, pan) => { calls.push(['play', name, volume, pan]); return name === 'tick'; },
    setVolume: volume => calls.push(['setVolume', volume]),
  }};
  try {
    assert.equal(Sound.play('tick'), true);
    assert.equal(Sound.play('page', {pan: -0.4}), false);
    Sound.play('tick', {volume: 0.5});
    Sound.setVolume(0);
    assert.deepEqual(calls, [['play', 'tick', 1, 0], ['play', 'page', 1, -0.4], ['play', 'tick', 0.5, 0],
      ['setVolume', 0]]);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('archive and download task IDs are isolated while sharing the polling timer', async () => {
  const originalInterval = globalThis.setInterval, originalClear = globalThis.clearInterval;
  let tick, networkSnapshots = [], archiveSnapshots = [];
  const cancelled = [];
  const extractions = [];
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {
    network: {download: () => 1, poll: () => networkSnapshots, cancel: id => cancelled.push(`network:${id}`)},
    archives: {extract: (...args) => { extractions.push(args); return 1; }, poll: () => archiveSnapshots, cancel: id => cancelled.push(`archives:${id}`)},
  };
  try {
    const download = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const archive = Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password: 'fixture-password'});
    assert.deepEqual(extractions, [[['/download0/file'], '/download0/out', 1024**4, 'fixture-password', false]]);
    assert.equal(Archives.streams, true);
    for (const password of [42, 'bad\0password', 'x'.repeat(1025)])
      assert.throws(() => Archives.extract({sources: ['/download0/file'], destination: '/download0/out', password}), /password/);
    assert.equal(extractions.length, 1);
    archive.cancel(); assert.deepEqual(cancelled, ['archives:1']);
    archiveSnapshots = [{id: 1, state: 'extracting', phase: 'finishing', written: 50, synced: 1, syncTotal: 3}]; tick();
    assert.deepEqual([archive.snapshot.phase, archive.snapshot.synced, archive.snapshot.syncTotal], ['finishing', 1, 3]);
    archiveSnapshots = [{id: 1, state: 'completed', written: 50, artifacts: ['game.ffpfsc']}]; tick();
    assert.deepEqual((await archive.done).artifacts, ['game.ffpfsc']);
    assert.equal(download.snapshot.state, 'queued');
    networkSnapshots = [{id: 1, state: 'completed', written: 100}]; tick();
    assert.equal((await download.done).written, 100);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('Packages installs and uninstalls through the host', async () => {
  let tick, snapshots = [];
  const calls = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {packages: {
    install: (...args) => { calls.push(['install', ...args]); return 1; },
    uninstall: (...args) => { calls.push(['uninstall', ...args]); return 2; },
    cancel: () => {},
    poll: () => snapshots,
  }};
  try {
    const install = Packages.install({path: '/data/pkg/counter.pkg', overwrite: true});
    assert.equal(install.id, 1);
    assert.deepEqual(calls, [['install', '/data/pkg/counter.pkg', '', true]]);
    snapshots = [{id: 1, state: 'installing', percent: 42, titleId: 'CUSA99060',
      contentId: 'UP0001-CUSA99060_00-PS4SAMPCOUNTER01', error: ''}];
    tick();
    assert.equal(install.snapshot.percent, 42);
    snapshots = [{id: 1, state: 'completed', percent: 100}];
    tick();
    assert.equal((await install.done).percent, 100);

    const remove = Packages.uninstall({titleId: 'CUSA99060'});
    assert.deepEqual(calls.slice(1), [['uninstall', 'CUSA99060']]);
    snapshots = [{id: 2, state: 'uninstalling', percent: 0, error: ''}];
    tick();
    assert.equal(remove.snapshot.state, 'uninstalling');
    snapshots = [{id: 2, state: 'completed', percent: 0}];
    tick();
    assert.equal((await remove.done).state, 'completed');
    assert.throws(() => Packages.uninstall({titleId: 'cusa99060'}), /nine-character/);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('Payloads checks BinLoader and sends through a network task', async () => {
  let tick, snapshots = [];
  const sends = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {payloads: {
    isAvailable: () => true,
    send: path => { sends.push(path); return 7; },
    cancel: () => {},
    poll: () => snapshots,
  }};
  try {
    assert.equal(Payloads.isAvailable(), true);
    const task = Payloads.send({path: '/data/pkg/payload.elf'});
    assert.equal(task.id, 7);
    assert.deepEqual(sends, ['/data/pkg/payload.elf']);
    snapshots = [{id: 7, state: 'sending', written: 64, total: 128, error: ''}];
    tick();
    assert.deepEqual([task.snapshot.state, task.snapshot.written, task.snapshot.total], ['sending', 64, 128]);
    snapshots = [{id: 7, state: 'completed', written: 128, total: 128}];
    tick();
    assert.equal((await task.done).state, 'completed');
    assert.throws(() => Payloads.send({}), /expected a payload path/);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('Dialog.progress updates and closes the system bar', async () => {
  let tick, snapshots = [];
  const calls = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {dialog: {
    startProgress: (...args) => { calls.push(['start', ...args]); return 4; },
    progressSet: (...args) => { calls.push(['set', ...args]); },
    progressClose: id => { calls.push(['close', id]); },
    poll: () => snapshots,
  }};
  try {
    const dialog = Dialog.progress({title: 'Progress', message: 'Starting'});
    assert.equal(dialog.id, 4);
    assert.deepEqual(calls, [['start', 'Progress', 'Starting']]);
    dialog.set(42, '42%');
    dialog.close();
    assert.deepEqual(calls.slice(1), [['set', 4, 42, '42%'], ['close', 4]]);
    snapshots = [{id: 4, state: 'completed', error: ''}];
    tick();
    assert.equal((await dialog.done).state, 'completed');
    assert.throws(() => dialog.set(101), /0 to 100/);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('Archives.create runs through the archive-create backend', async () => {
  let tick, snapshots = [];
  const calls = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => {};
  globalThis.__ps4ReactNative = {archiveCreate: {
    create: (...args) => { calls.push(args); return 9; },
    cancel: () => {},
    poll: () => snapshots,
  }};
  try {
    const task = Archives.create({sources: ['/app0/app.json'], destination: '/download0/out.zip'});
    assert.equal(task.id, 9);
    assert.deepEqual(calls, [[['/app0/app.json'], '/download0/out.zip', 'zip', false]]);
    snapshots = [{id: 9, state: 'writing', written: 10, entries: 1, error: ''}];
    tick();
    assert.deepEqual([task.snapshot.state, task.snapshot.entries], ['writing', 1]);
    snapshots = [{id: 9, state: 'completed', written: 20, entries: 1}];
    tick();
    assert.equal((await task.done).written, 20);
    assert.throws(() => Archives.create({sources: [], destination: '/download0/out.zip'}), /non-empty/);
    assert.throws(() => Archives.create({sources: ['/app0/app.json'], destination: '/download0/out.rar', format: 'rar'}), /zip.*tar/);
  } finally {
    globalThis.setInterval = originalInterval; globalThis.clearInterval = originalClear;
    delete globalThis.__ps4ReactNative;
  }
});

test('Sse connects, polls events and closes', () => {
  const calls = [];
  globalThis.__ps4ReactNative = {sse: {
    connect: (...args) => { calls.push(['connect', ...args]); return 11; },
    poll: id => { calls.push(['poll', id]); return {state: 'open', error: '', events: [
      {type: 'message', data: 'hello', id: '1'}]}; },
    close: id => { calls.push(['close', id]); },
  }};
  try {
    const stream = Sse.connect({url: 'https://example.com/events', headers: ['Accept: text/event-stream']});
    assert.equal(stream.id, 11);
    assert.deepEqual(stream.poll().events, [{type: 'message', data: 'hello', id: '1'}]);
    stream.close();
    assert.deepEqual(calls, [
      ['connect', 'https://example.com/events', ['Accept: text/event-stream']],
      ['poll', 11],
      ['close', 11],
    ]);
    assert.throws(() => Sse.connect({url: 'ftp://example.com'}), /http/);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('WebSocket connects, sends text and polls events', () => {
  const calls = [];
  globalThis.__ps4ReactNative = {websocket: {
    connect: url => { calls.push(['connect', url]); return 12; },
    send: (id, text) => { calls.push(['send', id, text]); return true; },
    poll: id => { calls.push(['poll', id]); return {state: 'open', error: '', events: [
      {type: 'open', data: '', binary: false}]}; },
    close: id => { calls.push(['close', id]); },
  }};
  try {
    const socket = WebSocket.connect('ws://192.168.0.8:8766/');
    assert.equal(socket.id, 12);
    assert.equal(socket.send('hello'), true);
    assert.equal(socket.poll().events[0].type, 'open');
    socket.close();
    assert.deepEqual(calls, [
      ['connect', 'ws://192.168.0.8:8766/'],
      ['send', 12, 'hello'],
      ['poll', 12],
      ['close', 12],
    ]);
    assert.throws(() => WebSocket.connect('http://example.com'), /ws/);
    assert.throws(() => socket.send(42), /string/);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('Controller.getState selects the player index', () => {
  const players = [];
  globalThis.__ps4ReactNative = {pad: {
    state: player => { players.push(player); return {connected: true, player}; },
  }};
  try {
    assert.equal(Controller.getState().player, 0);
    assert.equal(Controller.getState(2).player, 2);
    assert.deepEqual(players, [0, 2]);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});

test('Music controls the mixer deck', () => {
  const calls = [];
  globalThis.__ps4ReactNative = {music: {
    play: (path, volume) => { calls.push(['play', path, volume]); return 21; },
    pause: id => { calls.push(['pause', id]); },
    resume: id => { calls.push(['resume', id]); },
    stop: id => { calls.push(['stop', id]); },
    setVolume: volume => { calls.push(['volume', volume]); },
    status: id => { calls.push(['status', id]); return {state: 'playing', position: 1, duration: 3, error: ''}; },
  }};
  try {
    const music = Music.play({path: '/data/pkg/song.mp3', volume: 0.5});
    assert.equal(music.id, 21);
    music.pause();
    music.resume();
    assert.equal(music.status().state, 'playing');
    music.stop();
    Music.setVolume(0.25);
    assert.deepEqual(calls, [
      ['play', '/data/pkg/song.mp3', 0.5],
      ['pause', 21],
      ['resume', 21],
      ['status', 21],
      ['stop', 21],
      ['volume', 0.25],
    ]);
  } finally {
    delete globalThis.__ps4ReactNative;
  }
});
