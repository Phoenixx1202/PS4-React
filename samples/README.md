# Samples

Small React apps that exercise the native APIs. Each sample has its own
`app.json` (title, Title ID, Content ID) and is built into one PKG.

## Build a sample

```powershell
.\tools\build_ps4.ps1 -Sample counter
# -> build/PS4-React-counter.pkg

.\tools\build_ps4.ps1 -Sample network
.\tools\build_ps4.ps1 -Sample images
.\tools\build_ps4.ps1 -Sample files
.\tools\build_ps4.ps1 -Sample settings
.\tools\build_ps4.ps1 -Sample packages
.\tools\build_ps4.ps1 -Sample payloads
.\tools\build_ps4.ps1 -Sample archive
.\tools\build_ps4.ps1 -Sample device
.\tools\build_ps4.ps1 -Sample audio
.\tools\build_ps4.ps1 -Sample downloads
.\tools\build_ps4.ps1 -Sample sse
.\tools\build_ps4.ps1 -Sample websocket
.\tools\build_ps4.ps1 -Sample controllers
.\tools\build_ps4.ps1 -Sample music
```

On Linux:

```bash
./tools/build_ps4.sh --sample counter
```

Or build every sample into `samples-pkg/`:

```powershell
.\tools\build_samples.ps1
```

```bash
./tools/build_samples.sh
```

The default build still packages `app/` as `PS4-React-Starter.pkg`:

```powershell
.\tools\build_ps4.ps1
```

## Included

| Sample | What it shows |
|---|---|
| `counter` | React state, focus navigation, `Pressable`/`View` focus styles |
| `network` | `Http.request`, loading/error states and retry |
| `images` | `<Image>` with a remote PNG, `onLoad`/`onError` |
| `files` | `FileSystem.writeFile`, `readFile` and `readDir` under `/download0` |
| `settings` | `SaveData.mount` and `Keyboard.prompt`, persisting JSON in save data |
| `dialogs` | `Dialog.alert` and `Dialog.confirm` through the system message dialog |
| `packages` | `Packages.install`/`Packages.uninstall` with progress and errors; the sample is packaged as a privileged app |
| `payloads` | GoldHEN BinLoader availability and sending an ELF payload with confirmation |
| `archive` | `Archives.create` ZIP, `Archives.extract` and `Archives.inspect` |
| `device` | `DeviceInfo`, `Users` and `Notifications` |
| `audio` | Baked WAV playback with `Sound.play`, volume and pan |
| `downloads` | `Downloads.enqueue` progress, cancel and retry |
| `sse` | Server-Sent Events stream with event polling |
| `websocket` | ws/wss text messages, echo server and polling |
| `controllers` | Four player panels with `useGamepad(playerIndex)` |
| `music` | Local MP3 playback with the mixer music deck |

All samples share `samples/assets/Inter-Regular.ttf`; the bundler bakes only the
font sizes each app uses.

The GitHub Actions build creates the starter and every sample, and attaches all
of the PKGs to a release when a `v*` tag is pushed.

## More ideas

- `audio`: buttons that call `Sound.play` with the bundled WAVs.
- `downloads`: `Downloads.enqueue` progress, cancel and resume.
- `archive`: extract a small ZIP from `/app0` into `/download0`.
- `text`: CJK/Arabic strings with Noto fonts in `app/fonts`.
- `device`: `DeviceInfo`, `Users` and `Notifications`.
- `gallery`: a grid of remote images backed by `ScrollView`.
- `list`: `VirtualList` with a few hundred rows to test scrolling and focus.

Samples are standalone: copy one to `app/` if you want to use it as the default
build, or add new folders under `samples/` and run `-Sample <name>`.
