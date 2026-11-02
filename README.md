# PS4 React

<img width="256" height="256" alt="ReactPS4" src="https://github.com/user-attachments/assets/bff50afb-4d87-43fa-bd34-c0381147843d" />




React + JSX runtime for PlayStation 4 homebrew. It renders a real React tree
through Embedded React, runs JavaScript on QuickJS and presents a 1920×1080
framebuffer with `sceVideoOut` — no OpenGL or shader compilation.

The default build produces a single installable package:
**`PS4-React-Starter.pkg`**.

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
- **PKG installation** through `sceAppInstUtil` via `Packages.install`.
- **Save data** through `sceSaveData` and the system on-screen keyboard through
  `sceImeDialog`.
- **PKG packaging** validated with `PkgTool.Core` and a load-image checker.
- **Dev hot reload** over TCP when the build enables `-DPS4_REACT_DEV_SERVER=ON`.

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

On Linux, use the shell entry point:

```bash
./tools/build_ps4.sh
```

Output:

```text
build/PS4-React-Starter.pkg
```

The command builds only that package. `-NoPackage` produces the ELF/SELF without
packaging. `-DevServer` enables the TCP hot-reload receiver for local
development. The GitHub Actions workflow is
[`.github/workflows/build.yml`](.github/workflows/build.yml).

## Samples

[`samples/`](samples/) contains small apps for the native APIs. Build one with:

```powershell
.\tools\build_ps4.ps1 -Sample counter
# -> build/PS4-React-counter.pkg
```

On Linux, build one sample with:

```bash
./tools/build_ps4.sh --sample counter
```

Build every sample into `samples-pkg/`:

```powershell
.\tools\build_samples.ps1
```

```bash
./tools/build_samples.sh
```

Each sample's `app.json` supplies its title and Title ID. See
[`samples/README.md`](samples/README.md) for the list and ideas.

## Development and hot reload

Install a development build on the console with the receiver enabled:

```powershell
.\tools\build_ps4.ps1 -DevServer
```

```bash
./tools/build_ps4.sh --dev-server
```

Then, on the workstation, point the watcher at the console's IP:

```bash
node tools/dev_server.mjs --host 192.168.1.50
# sample:
node tools/dev_server.mjs --host 192.168.1.50 --sample counter
```

The console listens on TCP port 9001, validates each ERHR frame with CRC-32 and
reloads the JavaScript bundle in the running title. Assets (images, fonts,
sounds) are still compiled into the PKG; rebuild and reinstall when they
change. The receiver is off in release builds.

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
| L3 / R3 / touchpad | Exposed as actions |
| Options | Exit |

## Native APIs

JavaScript reaches the host through `@ps4-react/core`.

| API | Purpose |
|---|---|
| `Http.request` | HTTP/HTTPS request with headers, method and abort signal |
| `Downloads.enqueue` | Download to a file with ranges, resume and completion receipts |
| `<Image>` | Local baked assets and remote PNG/JPEG decoding |
| `Archives.extract` | ZIP, TAR and 7z extraction with a destination directory |
| `Sound` | Play baked WAV clips with volume and pan |
| `Notifications` / `Users` / `DeviceInfo` | Notifications, users and device information |
| `FileSystem` | Read, write, list, rename and inspect files |
| `Controller` / `useGamepad` | Light bar, vibration and pad state |
| `Packages.install` | Install a PKG through `sceAppInstUtil` |
| `SaveData` | Mount, unmount and delete PS4 save data |
| `Keyboard.prompt` | Open the system on-screen keyboard |
| `Fonts` / `BackHandler` / `Power` / `Linking` | Text language, exit, keep-awake and URLs |

`Payloads` and `BrowserCapture` are not part of the PS4 runtime. Message
dialogs and music streaming are not implemented yet.

Save data and the system keyboard are available from the same import:

```jsx
import {SaveData, Keyboard, FileSystem} from '@ps4-react/core';

const save = SaveData.mount({dirName: 'settings', title: 'My App'});
FileSystem.writeFile(`${save.path}/settings.json`, '{"name":"Player"}', {sync: true});

const result = await Keyboard.prompt({title: 'Player name', value: 'Player'}).done;
console.log(result.text);
```

The `settings` sample in [`samples/`](samples/) demonstrates both APIs.

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
- Message dialogs and music streaming are not implemented yet.
- `Packages.install`, `SaveData` and `Keyboard.prompt` use system services and
  still need validation on the firmware versions you target.
- Runtime text shaping needs the app to package the Noto fonts it uses.
- Hot reload only replaces JavaScript; asset changes require a new PKG.

## Licenses

The project is GPL-3.0-or-later with the attribution term in
[`LICENSE-ATTRIBUTION`](LICENSE-ATTRIBUTION). Third-party notices are in
[`NOTICE`](NOTICE) and [`licenses/`](licenses/).
