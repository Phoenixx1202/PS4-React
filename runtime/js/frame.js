// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
const callbacks = new Set();

// Hosts call this once per frame, after the pump and before the commit, with the time the frame
// advances: on the console a whole number of vblanks. Motion stepped here moves once per presented frame;
// a timer drifts against the display and skips or doubles frames.
globalThis.__ps4ReactFrame = elapsedMs => {
  for (const callback of callbacks) callback(elapsedMs);
};

/**
 * Internal: call `callback(elapsedMs)` every frame until the returned function is called.
 * @param {(elapsedMs: number) => void} callback
 */
export function onFrame(callback) {
  callbacks.add(callback);
  return () => { callbacks.delete(callback); };
}
