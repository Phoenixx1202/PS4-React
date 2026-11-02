// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// <Image>: bundled assets by name, and http(s) URLs loaded natively at the size they are drawn.
import {createElement, forwardRef, useLayoutEffect, useReducer, useRef} from 'react';
import {acquireImage, warmImages} from '../native.js';
import {createFocusable, isFocusable, resolveStyle} from './focusable.js';

const HostImage = createFocusable('Image');
const FITS = {cover: 0, contain: 1, stretch: 2, center: 3, repeat: 3};

/** The URL of a remote `source`, or null for a bundled asset name. */
function remoteUri(source) {
  const uri = typeof source === 'string' ? source : source?.uri;
  return typeof uri === 'string' && /^https?:\/\//i.test(uri) ? uri : null;
}

/** Width and height when the style fixes both in pixels, so loading need not wait for layout. */
function fixedBox(style) {
  let width, height;
  const resolved = resolveStyle(style, false, false);
  for (const part of Array.isArray(resolved) ? resolved.flat(Infinity) : [resolved]) {
    if (!part) continue;
    if (part.width !== undefined) width = part.width;
    if (part.height !== undefined) height = part.height;
  }
  return typeof width === 'number' && typeof height === 'number' && width >= 1 && height >= 1
    ? {width: Math.round(width), height: Math.round(height)} : null;
}

const OWN_PROPS = ['source', 'resizeMode', 'onLoad', 'onError', 'onLayout'];

// A store page or grid draws dozens of remote images, so each keeps one object across renders (its
// laid-out box, the engine image name, and stable layout and load callbacks), one hook to re-render
// with and one effect, in the Image component itself; the host element is rendered directly unless
// focus needs the focusable wrapper. A bundled image runs the same hooks and renders the asset.
function useImageElement(props, uri, forwardedRef) {
  const [, rerender] = useReducer(count => count + 1, 0);
  const self = useRef(null);
  if (!self.current) {
    const image = {
      props, laidOut: null, name: '',
      layout(event) {
        const {width, height} = event.layout;
        if (width >= 1 && height >= 1 && (image.laidOut?.width !== width || image.laidOut?.height !== height)) {
          image.laidOut = {width, height};
          rerender();
        }
        image.props.onLayout?.(event);
      },
      loaded(result) {
        if (result.name !== image.name) {
          image.name = result.name;
          rerender();
        }
        const {onLoad, onError, source} = image.props;
        if (result.state === 'ready') {
          onLoad?.({nativeEvent: {source: {uri: remoteUri(source), width: result.width, height: result.height}}});
        }
        if (result.state === 'failed') {
          if (onError) onError({nativeEvent: {error: result.error}});
          else console.warn(result.error);
        }
      },
    };
    self.current = image;
  }
  const image = self.current;
  image.props = props;
  const box = uri ? fixedBox(props.style) ?? image.laidOut : null;
  const fit = FITS[props.resizeMode ?? 'cover'] ?? 0;

  useLayoutEffect(() => {
    if (!uri || !box) return undefined;
    try {
      return acquireImage(uri, box.width, box.height, fit, false, image.loaded);
    } catch (error) {
      image.loaded({state: 'failed', name: '', error: error.message});
      return undefined;
    }
  }, [uri, box?.width, box?.height, fit]);

  if (!uri) return createElement(HostImage, {...props, ref: forwardedRef});
  const rest = {ref: forwardedRef, resizeMode: props.resizeMode ?? 'cover', imageName: image.name,
    onLayout: image.layout};
  for (const key in props) if (!OWN_PROPS.includes(key)) rest[key] = props[key];
  const plain = !isFocusable(rest) && !('focusable' in rest) && !rest.scrollAnchor && typeof rest.style !== 'function';
  return createElement(plain ? 'Image' : HostImage, rest);
}

/**
 * React Native's Image. `source` is a bundled asset (an imported file) or `{uri}` with an http(s)
 * URL, which is fetched and decoded off the render thread at the size the image is drawn; the
 * element draws only its own style (such as backgroundColor) until the image arrives.
 */
export const Image = forwardRef((props, ref) => useImageElement(props, remoteUri(props.source), ref));
Image.displayName = 'Image';

/**
 * Loads a URL into the image cache ahead of time for a box of `width` × `height` render pixels,
 * the size an `<Image>` with that `resizeMode` will draw it at. Holds no reference: the image stays
 * cached until the cache needs the room. Resolves when decoded; rejects with the load error.
 * @param {string} uri @param {{width: number, height: number, resizeMode?: string}} box
 * @returns {Promise<void>}
 */
/**
 * The most prominent vivid colour of an image as '#rrggbb', at full brightness, or null when the art
 * is grey or dark: for tinting an accent or the controller light bar. Decodes a small copy off the
 * render thread; a URL already fetched is not downloaded again. Rejects with the load error.
 * @param {string} uri
 * @returns {Promise<string | null>}
 */
Image.getColor = uri => new Promise((resolve, reject) => {
  let release = null, settled = false;
  release = acquireImage(uri, 48, 72, FITS.cover, true, result => {
    if (result.state === 'loading') return;
    settled = true;
    release?.();
    if (result.state === 'ready') resolve(result.color);
    else reject(new Error(result.error));
  });
  if (settled) release();
});

/**
 * Fills the disk cache with `uris` in the background, in order: a few connections, only while no
 * image waits to load, and only files up to 512 KiB. Nothing is decoded, so a later load still
 * decodes, from disk instead of the network. Each call replaces the previous list.
 * @param {string[]} uris
 */
Image.warm = uris => warmImages(uris);

Image.prefetch = (uri, {width, height, resizeMode = 'cover'}) => new Promise((resolve, reject) => {
  let release = null, settled = false;
  release = acquireImage(uri, Math.round(width), Math.round(height), FITS[resizeMode] ?? 0, true, result => {
    if (result.state === 'loading') return;
    settled = true;
    release?.();
    if (result.state === 'ready') resolve();
    else reject(new Error(result.error));
  });
  if (settled) release();
});
