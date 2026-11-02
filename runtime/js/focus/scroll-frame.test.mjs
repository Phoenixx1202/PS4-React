// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createFrame} from './scroll-frame.js';

/** A 1280×720 vertical ScrollView over a stand-in NativeUI.scrollTo that clamps to `max`. */
function setup(maxY) {
  const engine = {x: 0, y: 0, maxY};
  globalThis.screen = {width: 1280, height: 720};
  globalThis.NativeUI = {scrollTo(handle, x, y) {
    if (!Number.isNaN(x)) engine.x = x;
    if (!Number.isNaN(y)) engine.y = Math.min(Math.max(y, 0), engine.maxY);
    return [engine.x, engine.y, 0, engine.maxY];
  }};
  engine.deferred = 0;
  globalThis.__ps4ReactNative = {image: {defer: ms => { engine.deferred = ms; }}};
  const targets = [];
  const frame = createFrame(null);
  frame.props = {onScrollTarget: target => targets.push(target)};
  frame.ref(1);
  frame.onLayout({layout: {x: 0, y: 0, width: 1280, height: 720}});
  const settle = () => {
    for (let i = 0; i < 200 && frame.timer !== null; i++) globalThis.__ps4ReactFrame(1000 / 60);
  };
  return {engine, frame, targets, settle};
}

test('onScrollTarget reports each new focus-scroll target once, when the scroll starts', () => {
  const {engine, frame, targets, settle} = setup(5000);
  const card = {x: 0, y: 1000, width: 100, height: 100};
  frame.reveal(card, null);
  assert.deepEqual(targets, [{x: 0, y: 412}], 'the card ends a 32 px margin above the bottom');
  frame.reveal(card, null);
  assert.equal(targets.length, 1, 'an unchanged target is not reported again');
  settle();
  assert.equal(engine.y, 412);
  assert.equal(frame.y, 412);
  assert.equal(targets.length, 1, 'arriving is not a new target');
  assert.equal(engine.deferred, 150, 'image fetches wait while the view moves');
});

test('onScrollTarget reports where the scroll settled when shrinking content clamps it', () => {
  const {engine, frame, targets, settle} = setup(5000);
  frame.reveal({x: 0, y: 1000, width: 100, height: 100}, null);
  engine.maxY = 300;
  settle();
  assert.deepEqual(targets, [{x: 0, y: 412}, {x: 0, y: 300}]);
  assert.equal(frame.y, 300);
});
