# PS4 React

React + JSX runtime for PlayStation 4 homebrew. It renders a real React tree
through Embedded React, runs JavaScript on QuickJS and presents a 1920×1080
framebuffer with `sceVideoOut` — no OpenGL or shader compilation.

The build produces a single installable package: **`PS4-React-Starter.pkg`**.

## Features

- **1920×1080 presentation** with two linear VideoOut buffers and vsync pacing.
- **React 18 + JSX/TSX** bundled with esbuild and executed by QuickJS.
- **Controller input** (D-pad, sticks, Cross/Circle, L1/R1, L2/R2, Options).
- **Audio** through `sceAudioOut` (48 kHz stereo) and a real-time mixer.
- **Networking** with libcurl + mbedTLS: `Http` requests and `Downloads`.
- **Remote images** (PNG/JPEG) with an in-memory and disk cache.
- **Archives** (ZIP, TAR, 7z) through libarchive.
- **Runtime text shaping** for CJK, Arabic and Indic scripts with HarfBuzz and
  SheenBidi; apps package only the Noto fonts they use.
- **Native filesystem** under `/app0`, `/data` and `/download0`.
- **PKG packaging** validated with `PkgTool.Core` and a load-image checker.

## Requirements

Building on Windows uses WSL2 and a PacBrew rootfs with the OpenOrbis toolchain
and runtime libraries. The same steps work on Linux with the toolchain installed.

- Windows 10/11 + WSL2 (Ubuntu 22.04/24.04), or Linux.
- PacBrew rootfs containing:
  - OpenOrbis PS4 toolchain (`clang`, `create-fself`, `create-gp4`,
    `PkgTool.Core`);
  - runtime libraries: libcurl, mbedTLS, libarchive, bzip2, zlib, libpng;
  - HarfBuzz 14.6 source at `/workspace/harfbuzz` (the Linux/CI build downloads
    it automatically).
- Node.js 20+ and npm.
- Python 3, CMake, Git, `curl`, `xz`.

The first build fetches QuickJS-ng 0.15.0 and Embedded React at the revision in
[`dependencies.lock.json`](dependencies.lock.json), applies the pinned engine
patches and builds HarfBuzz for the PS4.

## Build

```powershell
.\tools\build_ps4.ps1
```

Output:

```text
build/PS4-React-Starter.pkg
```

The command builds only that package. `-NoPackage` produces the ELF/SELF without
packaging. The GitHub Actions workflow is
[`.github/workflows/build.yml`](.github/workflows/build.yml).

## Samples

[`samples/`](samples/) contains small apps for the native APIs. Build one with:

```powershell
.\tools\build_ps4.ps1 -Sample counter
# -> build/PS4-React-counter.pkg
```

Each sample's `app.json` supplies its title and Title ID. See
[`samples/README.md`](samples/README.md) for the list and ideas.

## Install

1. Copy `PS4-React-Starter.pkg` to a USB drive or `/data/pkg`.
2. On the PS4, open **GoldHEN → Install Package** and select the file.
3. Launch **PS4 React Starter**.

Title ID: `CUSA99059`.

The runtime log is written to `/data/PS4React/react.log` (fallback `/temp0`).

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | Move focus |
| Cross | Confirm |
| Circle | Back |
| L1 / R1 | Previous / next page |
| L2 / R2 | Previous / next section |
| Options | Exit |

## Native APIs

JavaScript reaches the host through `@ps4-react/core`.

| API | Purpose |
|---|---|
| `Http.request` | HTTP/HTTPS request with headers, method and abort signal |
| `Downloads.enqueue` | Download to a file with ranges, resume and completion receipts |
| `Images` / `<Image>` | Local baked assets and remote PNG/JPEG decoding |
| `Archives.extract` | ZIP, TAR and 7z extraction with a destination directory |
| `Sound` | Play baked WAV clips with volume and pan |
| `System` / `Users` / `Device` | Notifications, users and device information |
| `fs` | Read, write, list, rename and inspect files |

Paths:

- `/app0` — read-only package contents (the PKG root);
- `/download0` — writable application data, mapped to `/data/PS4React`;
- `/cache0` — image and cache data.

## Project layout

```text
app/                 React starter application (JSX, assets, sounds)
samples/             Example apps built with -Sample <name>
assets/              PKG icons and CA bundle
native/ps4/          PS4 host: VideoOut, input, audio, network, images,
                     archives, text shaping and packaging glue
runtime/js/          React runtime and focus/navigation components
tools/               build, bundling, packaging and load-image validation
patches/             pinned Embedded React patches
.github/workflows/   GitHub Actions build
```

## Status

Validated on PS4 hardware:

- VideoOut 1920×1080 at ~60 fps;
- React/QuickJS starter with focus, input and Options exit;
- `sceAudioOut` mixer and interface sounds;
- HTTPS requests (IPv4, CA bundle in `/app0`);
- downloads to `/download0` with completion receipts and resume;
- remote PNG decode at 256×256;
- ZIP extraction with two entries;
- Japanese, Simplified Chinese and Arabic shaping at 59.9 fps.

Known limitations:

- RAR/RAR5 extraction is not available yet; ZIP/TAR/7z are supported.
- PKG installation and payload APIs are not implemented.
- Runtime text shaping needs the app to package the Noto fonts it uses.

## Licenses

The project is GPL-3.0-or-later with the attribution term in
[`LICENSE-ATTRIBUTION`](LICENSE-ATTRIBUTION). Third-party notices are in
[`NOTICE`](NOTICE) and [`licenses/`](licenses/).
