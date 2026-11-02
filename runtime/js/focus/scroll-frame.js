// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The scrolling state behind a ScrollView: focus reveals, the eased per-frame scroll through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch) and `onScrollTarget`. Free of React so
// tests can drive it with a stand-in NativeUI.
import {onFrame} from '../frame.js';
import {deferImageFetches} from '../native.js';
import {anchorOffset, catchUpSpeed, revealOffset, scrollStep} from './geometry.js';

// 32 logical px of a 1280-wide layout, in screen px like the layout rectangles.
const MARGIN = 32 / 1280;
// The engine cannot animate a scroll offset, so JavaScript steps it on every presented frame (the
// host's frame callback, not a timer that drifts against the display), in whole pixels: up to 40
// logical px per 60 Hz frame, reached in four frames, then braking to stop on the target. A held
// key scrolls at that one steady speed, which also bounds the strip the engine rasterizes per frame.
const MAX_LOGICAL_PX_PER_FRAME = 40;
const ACCEL_LOGICAL_PX = 12;
const BRAKE_LOGICAL_PX = 4;
const FRAME_MS = 1000 / 60;
// Images wait while the view moves and this long after it stops, like a browser's lazy loading: art
// that only flies past is never fetched; what the view stops on (with the rows the list mounts ahead)
// starts then. Cached art still draws during the scroll.
const IMAGE_DEFER_MS = 150;

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref) ref.current = value;
}

/** @param {import('./manager.js').Frame | null} parent */
export function createFrame(parent) {
  const frame = {
    parent, x: 0, y: 0, props: {}, forwardedRef: null, handle: null, viewport: null, timer: null, target: null,
    speed: [0, 0], position: [0, 0], listeners: new Set(),
    /**
     * @param {import('./geometry.js').Rect} rect pre-scroll, like `viewport`
     * @param {import('./geometry.js').Rect | null} anchor its `scrollAnchor` ancestor in this frame, aligned
     *   on a vertical frame's y axis; the x axis and horizontal frames always move the least
     */
    reveal(rect, anchor) {
      const {viewport, handle} = frame;
      if (!viewport || handle == null) return;
      const [fromX, fromY, maxX, maxY] = NativeUI.scrollTo(handle, NaN, NaN);
      const margin = Math.round(screen.width * MARGIN);
      const x = revealOffset(frame.x, rect.x - viewport.x, rect.width, viewport.width, maxX, margin);
      const y = anchor && !frame.props.horizontal
        ? anchorOffset(frame.y, rect.y - viewport.y, rect.height, anchor.y - viewport.y, viewport.height, maxY, margin)
        : revealOffset(frame.y, rect.y - viewport.y, rect.height, viewport.height, maxY, margin);
      if (x === frame.x && y === frame.y) return;
      frame.x = x;
      frame.y = y;
      frame.animate(fromX, fromY, x, y);
      frame.notify();
      frame.props.onScrollTarget?.({x, y});
    },
    // A new target while scrolling only moves the target; the steps carry on at the current speed.
    animate(fromX, fromY, toX, toY) {
      frame.target = [Math.round(toX), Math.round(toY)];
      if (frame.timer !== null) return;
      const scale = screen.width / 1280;
      const [maxSpeed, accel, brake] = [MAX_LOGICAL_PX_PER_FRAME, ACCEL_LOGICAL_PX, BRAKE_LOGICAL_PX]
        .map(logical => Math.max(1, Math.round(logical * scale)));
      frame.position = [Math.round(fromX), Math.round(fromY)];
      frame.speed = [0, 0];
      frame.timer = onFrame(elapsedMs => {
        let [x, y] = frame.position;
        // A late frame advances the vblanks it covered, at most two, so a hitch is not followed by a jump.
        for (let n = Math.min(2, Math.max(1, Math.round(elapsedMs / FRAME_MS))); n > 0; n--) {
          let speedX, speedY;
          const capX = catchUpSpeed(frame.target[0] - x, frame.viewport.width, maxSpeed);
          const capY = catchUpSpeed(frame.target[1] - y, frame.viewport.height, maxSpeed);
          [x, speedX] = scrollStep(x, frame.speed[0], frame.target[0], capX, accel, brake);
          [y, speedY] = scrollStep(y, frame.speed[1], frame.target[1], capY, accel, brake);
          frame.speed = [speedX, speedY];
        }
        const [atX, atY] = NativeUI.scrollTo(frame.handle, x, y);
        deferImageFetches(IMAGE_DEFER_MS);
        // The engine stores offsets as floats. Far from the request means clamped by a content size that
        // changed since the target was chosen: settle where it stopped.
        if (Math.abs(atX - x) > 0.5) frame.target[0] = x = Math.round(atX);
        if (Math.abs(atY - y) > 0.5) frame.target[1] = y = Math.round(atY);
        frame.position = [x, y];
        if (x !== frame.target[0] || y !== frame.target[1]) return;
        frame.stop();
        const clamped = x !== frame.x || y !== frame.y;
        frame.x = x;
        frame.y = y;
        frame.notify();
        if (clamped) frame.props.onScrollTarget?.({x, y});
      });
    },
    /**
     * The content moved up by `dy` (a VirtualList dropping rows above the viewport, or down for a
     * negative `dy`): scrolls by the same amount so nothing moves on screen, mid-animation included.
     */
    shift(dy) {
      const [, atY] = NativeUI.scrollTo(frame.handle, NaN, NaN);
      NativeUI.scrollTo(frame.handle, NaN, atY - dy);
      frame.y -= dy;
      if (frame.timer === null) return;
      frame.position[1] -= dy;
      frame.target[1] -= dy;
    },
    // Viewport or offset changed; while animating, y is the target, so listeners see where it is going.
    notify() {
      for (const listener of frame.listeners) listener();
    },
    stop() {
      frame.timer?.();
      frame.timer = null;
    },
    onLayout(event) {
      frame.viewport = event.layout;
      frame.notify();
      frame.props.onLayout?.(event);
    },
    // While focus scrolling animates, x/y already hold its target; touch scrolling updates them live.
    onScroll(event) {
      if (frame.timer === null) {
        frame.x = event.scrollX;
        frame.y = event.scrollY;
        frame.notify();
      }
      frame.props.onScroll?.(event);
    },
    ref(handle) {
      frame.handle = handle;
      assignRef(frame.forwardedRef, handle);
    },
  };
  return frame;
}
