# PS4 React — Plano de implementação

> Última atualização: 2026-11-02
> Baseline validado em hardware; o restante é trabalho de feature.

Este arquivo é a lista de trabalho do runtime. Marque `[x]` somente quando o
critério de aceite for cumprido, não apenas quando o código existir.

## Como marcar

- `[x]` concluído e validado (hardware ou CI + testes);
- `[ ]` pendente;
- `[ ] **WIP**` em andamento (mantenha desmarcado até concluir);
- ao concluir, acrescente o commit, a data e o console/firmware, por exemplo:
  `— abc1234, 2026-11-02, PS4 9.00 / GoldHEN 2.4b18.6`.

Critério de conclusão padrão:

1. código integrado ao build padrão;
2. teste automatizado (Node e/ou nativo) ou teste de hardware documentado;
3. CI verde;
4. sample ou app de teste exercitando a feature;
5. documentação/`README` atualizados;
6. sem logs versionados;
7. erros, cancelamento e limpeza (suspend, save data, threads) tratados.

## Baseline já validado no hardware

### Plataforma e build

- [x] Build PacBrew/OpenOrbis, `create-fself`, `create-gp4` e PKG;
- [x] CI de testes, starter, samples, artefatos e release por tag;
- [x] PKG registrado como BigApp (`CATEGORY=gd`, `APP_TYPE=1`);
- [x] heap dlmalloc próprio (crt1 + tabela de malloc + `--wrap`) sobre
  system flexible memory;
- [x] saída limpa por Options: `sceKernelGetAppInfo` +
  `sceSystemServiceKillApp`, com unmount do save data antes do suspend;
- [x] log em `/data/PS4React/react.log` e `/data/PS4React/heap.log`, sem
  versionamento.

### Runtime atual

- [x] VideoOut 1920×1080, dois buffers lineares, ~60 fps;
- [x] React 18 + JSX/TSX + QuickJS e bundler esbuild;
- [x] input de controle (D-pad, sticks, Cross/Circle, L1/R1, L2/R2, Options,
  L3/R3, touchpad);
- [x] áudio `sceAudioOut` 48 kHz estéreo e mixer de sons WAV;
- [x] `Http.request` HTTP/HTTPS com CA bundle;
- [x] `Downloads.enqueue` com ranges, retomada e recibos;
- [x] imagens remotas PNG/JPEG com cache em memória e disco;
- [x] extração ZIP validada; TAR/7z implementados;
- [x] text shaping CJK/árabe com HarfBuzz + SheenBidi; Indic implementado;
- [x] `FileSystem` em `/app0`, `/data` e `/download0`;
- [x] save data (`sceSaveDataMount2`, reparo de diretório quebrado,
  persistência JSON) e unmount no exit;
- [x] teclado do sistema (IME), com Cross confirmando via `ATTRIBUTE=2`;
- [x] `Packages.install` implementado (`sceAppInstUtilAppInstallPkg`);
- [x] hot reload de JavaScript por TCP (dev server).

## Fase 1 — Completar o runtime (P0)

### 1.1 Eventos de sistema e ciclo de vida

- [x] bombear `sceSystemServiceReceiveEvent` no loop principal (validado no
  hardware; este firmware não entrega `OnResume`, então o estado vem do poll de
  `sceSystemServiceGetStatus`);
- [x] detectar `background`/`active` e desmontar/remontar o save data nessa
  transição (validado no sample de settings);
- [ ] pausar/retomar áudio e rede, e suspender/retomar o render;
- [x] expor `AppState` no JS (`active`/`background`, listener `change`);
- [ ] tratar foco/desfoque, login/logout e troca de usuário;
- [ ] liberar IME/common dialog antes de suspender (save data já resolvido);
- [ ] teste de hardware: rest mode direto e logout sem crash.

**Aceite:** suspend/resume em ~1 s, sem perda de save, áudio/rede retomam, JS
recebe os eventos. Background/active e save data validados; rest mode direto e
áudio/rede pendentes.

### 1.2 Diálogos do sistema

- [x] `Dialog.alert`/`Dialog.confirm` via `sceMsgDialog`;
- [x] barra de progresso (`sceMsgDialogProgressBar*`) com `Dialog.progress`
  (validada no sample dialogs);
- [x] ciclo `sceMsgDialogInitialize/Open/GetStatus/GetResult/Terminate`;
- [x] ignorar o pad do app enquanto o diálogo ou o IME está aberto
  (tratamento de suspend/foco fica na 1.1);
- [x] usar Cross/O conforme `ATTRIBUTE=2`;
- [x] sample `dialogs`.

**Aceite:** abrir, confirmar, cancelar e fechar; sem travar o render; mensagem
de erro do host consegue usar o diálogo do sistema. Núcleo e barra de progresso
validados no hardware.

### 1.3 `Packages` completo

- [x] validar instalação real de um sample PKG em `/data/pkg` (counter via BGFT);
- [x] progresso, erros e título instalado no JS; cancelamento implementado, mas
  ainda sem teste de hardware;
- [x] `Packages.uninstall` com `sceAppInstUtilAppUnInstall`;
- [x] instalar por cima, desinstalar e reabrir;
- [ ] teste com PKG grande e PKG inválido.

**Requisito de empacotamento:** `libSceAppInstUtil` só é liberada para apps
privilegiados e o GoldHEN transforma imports não resolvidos em crash. O app
precisa de `"privileged": true` (content ID `IV0000`, `CATEGORY=gde`,
`ATTRIBUTE=32`, SELF `paid 0x3800000000000011`), carregar
`libSceAppInstUtil`/`libSceBgft` por `sceSysmoduleLoadModuleInternal` e resolver
as funções com `sceKernelDlsym`. A instalação usa o BGFT
(`sceBgftServiceIntDownloadRegisterTaskByStorageEx`), não
`sceAppInstUtilAppInstallPkg`.

**Aceite:** instalar/desinstalar um sample sem crash e com estado correto no
app.db. Validado no hardware; PKG grande e inválido pendentes.

### 1.4 Relaunch

- [x] implementar `BackHandler.exitApp({relaunch: true})`;
- [x] mecanismo: `sceSystemServiceLoadExec("/app0/eboot.bin", nullptr)`; o
  `sceLncUtilLaunchApp` não é exportado no processo e o import direto crasha
  (trap do GoldHEN);
- [x] evitar corrida com o `KillApp`: o `LoadExec` substitui o processo
  (`Syscore App: Kill for LoadExec` seguido de novo `EXEC /app0/eboot.bin`);
- [x] teste de hardware.

**Aceite:** fecha e reabre o mesmo título sem passar pelo dashboard manual.
Validado no hardware; o log recomeça do zero após o relaunch.

### 1.5 Save data: polimento

- [x] desmontar no background e remontar no active/resume (validado; o suspend
  block do `SceSaveData` deixa de travar o suspend do sistema);
- [x] adicionar `save_data.png` em `sce_sys` (validado; o KLog não acusa mais
  ausente);
- [x] ícone, título e subtítulo no diálogo de save (`SaveData.mount` recebe
  `title`/`subtitle` e o ícone vem de `sce_sys/save_data.png`);
- [ ] tratar crash sem deixar mount pendurado;
- [ ] limite de mounts e mensagens de erro melhores (limite de 8 implementado,
  sem teste de hardware).

### 1.6 Fontes e texto

- [ ] `Fonts.load(path/url)` para TTF/OTF em runtime;
- [ ] fallback de fontes (lista de famílias e glifos ausentes);
- [x] validar shaping Indic no hardware: hindi e bengali renderizados no sample
  `text` com subsets Noto, junto de japonês, chinês e árabe; ordem RTL e
  parágrafo longo conferidos no hardware;
- [x] documentar fontes empacotadas versus carregadas em `docs/TEXT.md` (JP, SC,
  árabe e Indic validados no hardware no sample `text`);

### 1.7 Multi-controle e multi-usuário

- [x] enumerar usuários e abrir um pad por usuário (validado com um usuário;
  até 4 pads);
- [x] `Controller`/`useGamepad(playerIndex)` e troca de jogador (validado);
- [ ] eventos de login/logout;
- [ ] teste com dois controles (sample controllers criado; falta um segundo
  controle).

### 1.8 Rede avançada

- [x] WebSocket texto (`ws://`/`wss://`) com handshake, framing, ping/pong,
  close e fila limitada; validado com servidor de eco local;
- [x] SSE/streaming HTTP para respostas longas (cliente `Sse` validado com
  servidor local; fila limitada a 256 eventos);
- [ ] binário de saída e reconexão automática do WebSocket;
- [ ] limites de memória e backpressure (fila limitada implementada; falta
  teste de carga);
- [ ] servidor de teste local e testes automatizados (teste JS de `Sse` feito).

### 1.9 Arquivos compactados

- [x] `Archives.create` (ZIP/TAR) com entradas, progresso e cancelamento;
  ZIP validado no hardware, TAR implementado mas ainda sem teste;
- [ ] validar TAR e 7z no hardware (ZIP criar/extrair já validado);
- [ ] RAR5 não criptografado e RAR criptografado (ver 2.4);
- [ ] testes de path traversal, symlink e limites.

### 1.10 Hot reload de assets

- [ ] estender o protocolo do dev server para imagens, fontes e sons;
- [ ] recarregar cache de imagens, shaper e mixer sem reinstalar;
- [ ] fallback para rebuild quando o asset não for recarregável.

### 1.11 Samples que faltam

- [x] `audio` (Sound.play) — sample com WAV empacotado validado;
- [x] `downloads` (progresso/cancelar/retomar) — sample validado;
- [x] `archive` (extrair/criar) — ZIP criar/inspecionar/extrair validado;
- [x] `text` (CJK/árabe/Indic) - sample com subsets Noto empacotados em
  `fonts/`, parágrafo longo e ordem RTL; validado no hardware (PKG
  `750d1ab986fdcafb96f633323057dff2d399693f2dd4f7d73a4d75e3b1182b2e`);
- [x] `device` (DeviceInfo/Users/Notifications) — validado, incluindo
  temperatura via `libkernel_sys` e memória flexível disponível;
- [x] `gallery` (imagens + ScrollView) — 9 PNGs empacotados, grid de 3
  colunas e scroll pelo foco; validado no hardware;
- [x] `list` (VirtualList com centenas de linhas) — 1000 linhas, janela ao
  redor do viewport e scroll pelo foco; validado no hardware.

## Fase 2 — Mídia (P1)

### 2.1 Música e streaming

- [x] decoder MP3 (minimp3) e OGG Vorbis (`stb_vorbis`) com resample para
  48 kHz; Opus pendente (depende da API `libSceOpusCeltDec`);
- [x] `Music.play(file|url)`, pause/resume/stop/volume: arquivo local validado;
  URL HTTP(S) MP3/OGG decodificada em fluxo com ring buffer de 1 MiB (sem baixar
  a faixa inteira) e validada no hardware. `seek` funciona em arquivo local
  MP3/OGG; stream HTTP ainda não aceita seek;
- [x] playlists no sample (`Previous`/`Next`/auto-avanço) e metadados ID3
  (`title`/`artist`/`album`) no status; capa de álbum MP3 (APIC) e OGG Vorbis
  (`METADATA_BLOCK_PICTURE`) decodificadas, registradas no engine e exibidas no
  sample. Crossfade de 1,5 s com dois decks validado no hardware;
- [x] crossfade usando os dois decks e o `stream_ring` do mixer: música A no
  stream 0, áudio de vídeo no 1 e música B no 2; a playlist inicia a próxima
  faixa antes do fim do arquivo local e troca manual também sobrepõe;
- [ ] BGM do sistema (`sceMusicPlayerService`) quando disponível;
- [x] sample `music` (MP3 local e HTTP validados no hardware).

**Aceite:** tocar MP3 local e stream HTTP com crossfade, sem underrun e com
CPU baixa. MP3 e OGG Vorbis locais/HTTP e crossfade validados; Opus pendente.

### 2.2 Vídeo

- [x] spike `sceAvPlayer` local MP4 (H.264): init/PostInit/AddSource/Ready/
  EnableStream/Start, frames NV12 via `GetVideoDataEx` com pitch e crop;
  validado no hardware em 480p (30 fps) com decoder de software;
- [x] API `Video` (play/pause/resume/seek/stop/setRect/status) e sample
  `video` com overlay no framebuffer do engine; falta o componente `<Video>`;
- [x] controle play/pause/resume/seek (ms)/stop e fechamento limpo pelo
  handshake `sceSystemServiceLoadExec("exit", nullptr)`;
- [x] áudio AAC: `sceAvPlayerGetAudioData` (mono S16 48 kHz) convertido para o
  mixer em um thread dedicado, com resample linear; pause/resume acompanham o
  áudio. Validado no hardware no sample 480p;
- [ ] 720p/1080p: o caminho Vulkan já entrega os frames como texturas Y/UV sem
  conversão na CPU, então o custo restante é o decoder de hardware, que exige
  `libSceGnmDriver` e não pode fechar por `KillApp` (suspend timeout); a saída
  continua usando o handshake `LoadExec("exit")`;
- [x] caminho de textura GPU/YUV (evita a conversão CPU e o custo de ler
  WC_GARLIC): texturas `R8`/`R8G8`, upload apenas em frame novo e composição no
  fragment shader do Vulkan com o mesmo letterbox do caminho software; validado
  no hardware com o sample 480p (cores, posição, pause/seek/stop e saída limpa);
- [x] sample `video`.

**Aceite:** MP4 H.264 1080p com áudio sincronizado, seek e sem leak. 480p
validado nos caminhos software e GPU/YUV; 720p/1080p e decoder de hardware
pendentes.

### 2.3 Imagens

- [ ] GIF animado e WebP/AVIF conforme suporte do decoder;
- [ ] orçamento de memória do cache e política de despejo;
- [ ] testes de imagem grande/truncada e cancelamento.

### 2.4 RAR5 / RAR criptografado

- [ ] escolher decoder (libarchive mais novo, UnRAR ou 7-Zip) com revisão de
  licença;
- [ ] integrar RAR5 sem senha e com senha;
- [ ] validar checksums, volumes e path traversal;
- [ ] sample/entrada de teste.

**Aceite:** extrair RAR5 e RAR criptografado; ZIP/TAR/7z sem regressão.

### 2.5 Câmera e microfone (opcional)

- [ ] `sceCamera` (preview e captura) com permissões e ausência de device;
- [ ] `sceAudioIn` (nível/stream) e sample;
- [ ] orçamento de banda e memória.

## Fase 3 — Browser e Payloads (P1/P2, com spike)

### 3.1 BrowserCapture

- [ ] spike: definir escopo — browser do sistema, WebKit embutido ou browser
  remoto capturado por WebSocket/vídeo;
- [ ] protótipo da opção viável;
- [ ] API React (`BrowserView`/`BrowserCapture`) e fallback;
- [ ] decisão documentada se alguma opção não for viável no PS4.

**Aceite:** protótipo navegando e exibindo uma página, ou decisão assinada com
limitação técnica/legal.

### 3.2 Payloads

- [x] modelo seguro: cliente BinLoader (GoldHEN, porta 9090) para payloads do
  próprio usuário, sem execução arbitrária no processo do app;
- [x] API `Payloads.send(path)`, progresso, erro e `isAvailable` (lê o
  `config.ini`; uma conexão TCP vazia derruba o accept loop do BinLoader);
- [x] limites de tamanho (64 MiB), ELF obrigatório, origem resolvida e
  confirmação do usuário no sample;
- [ ] auditoria de segurança formal e documentação legal;
- [x] teste com payload de exemplo (`ps4-manager` ELF, enviado sem crash).

**Aceite:** enviar um payload de teste ao BinLoader sem crash; nenhuma execução
silenciosa. Validado no hardware; a auditoria formal fica pendente.

## Fase 4 — GPU, 4K e HDR (P2)

### 4.1 Backend GPU

- [x] spike Vulkan/RADV com o SDK do PS4SX2: instância/device (`AMD Liverpool
  (PS4) (RADV BONAIRE)`), imagem linear 1920x1080 host-visible, registro
  direto no `sceVideoOutRegisterBuffers` e flip com clear vermelho/verde/azul
  validados no hardware (probe local, ainda fora do runtime React);
- [x] abstração `Renderer` com implementação software e apresentação GPU
  (Piglet/EGL) opt-in por `"gpu": true`, com fallback software; validada no
  hardware com shaders pré-compilados da Sony (`texmap`);
- [x] runtime React completo (engine, QuickJS, bridge e APIs nativas) no
  toolchain PS4SX2 (clang-21/libc++21 + Mesa/RADV), com a UI apresentada por
  textura Vulkan 1080p em dois buffers de scanout alternados; validado no
  hardware com 2256 frames a 59,9 fps e sem falhas de `present`. O mesmo build
  usa o text shaper real (HarfBuzz + SheenBidi + `hb-raster` compilados com
  libc++21); japonês, chinês e árabe foram validados no hardware;
- [ ] atlas de textura, batching, scissor, blend e shaders para a UI 2D;
  bloqueado com os shaders pré-compilados da Sony: `texmap/v_0`, `v_1` e `v_2`
  derivam a UV da posição do vértice via `u_textureSpaceMatrix`, então cada
  quad exigiria um draw call próprio. O probe em hardware confirmou que nenhuma
  variante aceita UV por vértice. Para destravar seria preciso um shader PSSL
  próprio (ShaccCg) ou GNM; o caminho validado continua sendo composição
  software + upload GPU;
- [ ] textura de vídeo/imagem e upload assíncrono;
- [x] manter o software como fallback e para CI;
- [ ] comparar fps, memória e qualidade nos samples (GPU ~32 fps contra ~40 fps
  software no sample gpu; o upload/conversão por frame ainda pesa).

**Aceite:** starter e samples visualmente equivalentes a 1080p60 no GPU, com
fallback software funcionando.

### 4.2 4K e HDR

- [ ] VideoOut 3840×2160 no PS4 Pro (depende do backend GPU);
- [ ] scaling/safe area e UI responsiva;
- [ ] metadados HDR e validação em TV compatível;
- [ ] fallback 1080p e detecção de modo.

**Aceite:** UI 4K estável (30/60 fps conforme o backend) e HDR correto, sem
regressão em 1080p.

### 4.3 Performance

- [ ] orçamento de memória (heap, direct memory, texturas, vídeo);
- [ ] profiling de CPU/GPU e termais;
- [ ] metas de fps/load time por sample.

## Fase 5 — Plataforma e firmware

- [ ] matriz de firmware (5.05/6.72/9.00/11.00 conforme consoles disponíveis);
- [ ] GoldHEN 2.4b e versões suportadas;
- [ ] PS4 Fat/Slim/Pro;
- [ ] comportamento de `KillApp`, save data, IME e suspend por firmware;
- [ ] documentar diferenças e workarounds;
- [ ] checklist de teste manual por release.

## Fase 6 — Qualidade, testes e documentação

- [ ] testes nativos host-side para parsers (rede, arquivos, arquivos
  compactados, download);
- [ ] fuzzing de ZIP/TAR/7z/RAR5, HTTP e imagens;
- [ ] sanitizers/ASan no host quando possível;
- [ ] CI: matriz de build, validação de PKG e artefatos;
- [ ] documentação da API e dos samples;
- [ ] guia de troubleshooting e matriz de firmware;
- [ ] orçamento de memória/fps documentado;
- [ ] auditoria de licenças para novos decoders/backends;
- [ ] revisão de logs: nunca versionar `/data/PS4React` nem capturas de KLog.

## Backlog de pesquisa (spikes)

- [ ] BrowserCapture: viabilidade de WebKit/browser remoto no PS4;
- [ ] Payloads: modelo de execução seguro (BinLoader versus in-process);
- [ ] GPU: Piglet/OpenGL ES versus GNM;
- [ ] Vídeo: `sceAvPlayer` e formatos suportados;
- [ ] 4K/HDR: requisitos de VideoOut no Pro;
- [ ] RAR5: decoder e licença;
- [ ] Música: formatos e BGM do sistema.

## Ordem recomendada

1. **Fase 1.1–1.5** — eventos, diálogos, Packages, relaunch, save data.
2. **Fase 1.6–1.11** — fontes, multi-pad, rede avançada, arquivos, hot reload,
   samples.
3. **Fase 2** — música, vídeo, imagens, RAR5, câmera/mic.
4. **Fase 3** — BrowserCapture e Payloads (spikes primeiro).
5. **Fase 4** — GPU, 4K/HDR e performance.
6. **Fase 5–6** — matriz de firmware, testes, documentação e auditoria.

## Riscos e decisões abertas

- **BrowserCapture** pode não ser viável no PS4; o spike decide antes de
  prometer a API.
- **Payloads** exigem modelo de segurança/legal; a opção segura é BinLoader,
  não execução arbitrária no processo.
- **GPU** muda a arquitetura “sem OpenGL” do projeto; manter o software como
  fallback e medir o custo.
- **Vídeo** depende de `sceAvPlayer` e, para 1080p60, do backend GPU.
- **RAR5** tem licença restritiva em algumas implementações; revisar antes.
- **Firmware** pode alterar `LoadExec`/`KillApp`/suspend; validar por versão.
- **Memória**: o heap atual é 512 MiB; GPU/vídeo/4K exigem orçamento novo.
- **CI não roda no console**: a validação final é manual e deve ser registrada.

## Registro de conclusão

Ao marcar uma tarefa, acrescente uma linha aqui com commit, data, console e
firmware/GoldHEN:

```text
- 2026-11-02 - baseline: big app, heap, exit, save data, teclado e IME
  validados no hardware; commit 5d0e9d8.
- 2026-11-02 - Dialog.alert/Dialog.confirm validados no hardware (sample
  dialogs, PKG 6fef91d3681a7a929193c8159c5d1c4f449927aa10fe5ae4430bfe6fe7bf1d9c);
  commit local d1bca53, sem push.
- 2026-11-02 - pump de eventos/AppState e suspend do save data validados no
  hardware (settings): `app: event background` -> `saveData: suspended
  settings` -> `active` -> `resumed settings at /savedata0`; PKG
  93a23790d01a7925156bd7c6eb786cfd61aa38b054a812f9e58d906ca9eb9e65.
  `OnResume` não é entregue por este firmware; o poll de status cobre.
  Exit após resume ainda a confirmar. Sem push.
- 2026-11-03 - exit após resume confirmado no hardware: Options ->
  `GetAppInfo` -> kill, sandbox removido, sem coredump.
- 2026-11-03 - `Packages` validado no hardware (PS4 5.05/GoldHEN, sample
  packages): instalação do counter por BGFT após carregar
  `libSceAppInstUtil`/`libSceBgft` com `sceSysmoduleLoadModuleInternal`,
  instalar por cima e desinstalar com `sceAppInstUtilAppUnInstall`; PKG
  `0ee1b0ba5e54aa3a1a9d657b0c3b076a01703fdecceff654a85cc856b4f9c0e9`,
  counter `35e2029a403843994edc8c5cbfeb383f8341deba1a758f964a534477ff7adbcd`.
  Log: `packages: installed CUSA99060 from /data/pkg/PS4-React-counter.pkg`
  e `packages: uninstalled CUSA99060`. Commit local `783c23a`, sem push.
- 2026-11-03 - relaunch validado no hardware (sample packages): o app fecha e
  reabre pelo `sceSystemServiceLoadExec("/app0/eboot.bin")`; KLog mostra
  `Kill for LoadExec` e um novo `EXEC /app0/eboot.bin`, e o `react.log` recomeça
  do zero. `sceLncUtilLaunchApp` não é exportado no processo e o import direto
  crasha. PKG `8210a1ce35ba4b3abf9f13dd94e69bb7db53927ed9ddbb70eac5288d74e6067b`;
  commit local `eb13b35`, sem push.
- 2026-11-03 - Payloads/BinLoader validado no hardware (sample payloads):
  `payloads: sending /data/pkg/payload.elf (193728 bytes)` ->
  `payloads: sent /data/pkg/payload.elf` com o `ps4-manager` ELF. O
  `isAvailable` lê `/data/GoldHEN/config.ini`; uma conexão TCP vazia pode
  derrubar o accept loop do BinLoader, então o envio é o teste real. PKG
  `8318d7fd67dc8b0075e3fb3f819641c7eb992c4c0fae231ff1172dc4bf70a5ee`,
  commit local `7bca09e`, sem push.
- 2026-11-03 - barra de progresso do Dialog validada no hardware (sample
  dialogs, PKG `fcb61825c4d83f9c4ad603da5bab5daf04ddf6cd410638fad3019bb00e426d9b`);
  commit local `9d72fc2`.
- 2026-11-03 - save data com `sce_sys/save_data.png` validado no hardware
  (settings, PKG `23b166e533fbf693d27c64a73c5840cc98ddc79a193b7ee1f045583408bc6133`):
  KLog mostra `Mounted > ... mountPoint=/savedata0`, unmount no close e não
  acusa mais `save_data.png` ausente. Commit local `475b9b2`.
- 2026-11-03 - `Archives.create` ZIP validado no hardware (sample archive, PKG
  `85fc1b141760c59acf34a02026d5ed32094308bbf5bd7d0e65b59097b3909e68`):
  `archives.create: wrote /data/PS4React/sample.zip (1 entries, 188905 bytes)`;
  inspect, extract e verify do `ca-bundle.crt` (188905 bytes) funcionaram.
  TAR implementado, mas ainda sem teste. Commit local `a1a1f4e`.
- 2026-11-03 - sample device validado no hardware (PKG
  `aa76aa5bc0e3dff7de9d93da7bee521169d27516e882c80d887e048c85c12168`):
  memória flexível disponível via `sceKernelAvailableFlexibleMemorySize` e
  temperaturas de CPU/SoC via `sceKernelGetCpuTemperature`/
  `sceKernelGetSocSensorTemperature` resolvidas de `libkernel_sys` com
  `sceKernelDlsym`. Commit local `41efc60`.
- 2026-11-03 - samples audio e downloads validados no hardware: WAV empacotado
  (`sound=yes`, beep/pan/volume) e download de 10 MB com progresso/cancelar/
  retry. PKGs `26fdb4bf265c3093421bc3cd4dfdc22dd3b8c596ac26ee2f85d74f4453b5bd0a`
  e `6c60a439a73f204c121ac944ec95dec60eafce08f5fa3a42b3b097f79e62348f`;
  commits locais `b3087c5` e `0c69890`.
- 2026-11-03 - SSE validado no hardware (sample sse, PKG
  `d0e0960f7118955baa99a20943d86d73c49933e8d265a5e2d98a55672d9ecba0`):
  `sse: connecting http://192.168.0.8:8765/events` -> `poll state=open` ->
  `closed`, com os eventos `tick` exibidos. Endpoint público fechou sem
  entregar eventos, então o teste usou um servidor local no PC. Commit local
  `b185c07`.
- 2026-11-03 - WebSocket texto validado no hardware (sample websocket, PKG
  `7701b605b3961e5fe85321a7a50da5d6cb02166dfd2613126404bf91e1b48d49`):
  `websocket: open ws://192.168.0.8:8766/` -> `poll state=open` -> envio
  `hello ...` ecoado e `closed`. Commit local `91d74e9`. Binário de saída e
  reconexão automática pendentes.
- 2026-11-03 - música MP3 local validada no hardware (sample music, PKG
  `a8959029ad4a62334f5c0d8641aac823376ccd555d89ad6087265570fb00b98a`):
  `music: /data/pkg/music.mp3 44100 Hz 2 ch, 16436736 frames`, play/pause/
  resume/stop/volume funcionaram. Commit local `59346e9`. OGG/Opus, HTTP,
  seek, playlists/crossfade e metadados pendentes.
- 2026-11-03 - multi-controle validado no hardware (sample controllers, PKG
  `2c80a206ce4ad8dfdbcaa3fb8f5f922d62635a1ddb2706c35c7d5f2799156862`):
  `PS4 React Starter: pads=1 users=1`, Player 1 conectado e estados por
  jogador via `Controller.getState`/`useGamepad(playerIndex)`. Commit local
  `5f728af`. Falta o teste com dois controles e eventos de login/logout.
- 2026-11-03 - GPU/Piglet validado no hardware (sample gpu, PKG
  `435b84596a52ea9911306b787c6f9b59f1532096e0f24a2c91f0485e03783de2`):
  `gpu: GL_VERSION=OpenGL ES 2.0 Piglet`, shaders pré-compilados
  `texmap/v_2.vert`/`f_2.frag` via `glShaderBinary`, `gpu presentation=yes` e
  primeiro frame apresentado. O sample é IV0000/gde (compositor VSH) e usa
  heap de 128 MiB. ~32 fps por causa da conversão/upload por frame; batching e
  otimização pendentes. Commit local `9a3928f`.
- 2026-11-03 - tentativa de backend GPU de UI (FBO + atlas + batching) foi
  revertida: os shaders `texmap/v_0`, `v_1` e `v_2` calculam a UV a partir da
  posição, então não aceitam UV por vértice e cada operação exigiria um draw
  call. O probe em hardware confirmou o bloqueio; o caminho software +
  apresentação GPU (PKG `35b687e14d5aa2f9e375a57b86a9d91a9e5263ba9c57676391e1923ae4a85f5f`)
  foi restaurado e validado. Sem push.
- 2026-11-03 - vídeo `sceAvPlayer` validado no hardware (sample video,
  PS4 5.05/GoldHEN): `init`, `PostInit`, `AddSource`, evento Ready, stream de
  vídeo habilitada e `Start`; frames 864x480, `pitch=896`, `crop_right=42`,
  NV12 8-bit; conversão para ARGB e blit no framebuffer do engine a 30 fps.
  O decoder precisa de `libSceGnmDriver` pré-carregado (senão GoldHEN mata a
  thread com `PRX_NOT_RESOLVED_FUNCTION`). O `KillApp` suspende o título e
  espera um submit-done de GPU que não chega, estourando
  `CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC`; a saída limpa usa o handshake
  oficial `sceSystemServiceLoadExec("exit", nullptr)`. Seek em milissegundos,
  pause/resume/stop e fechamento validados no hardware; PKG
  `f4cb215b77cc84c3b42b61548c8427149446b76c8f4924d3e069fccc2804d839`.
  Áudio AAC (mono S16 48 kHz) implementado e ligado ao mixer em thread
  dedicado; pause/resume/seek/fechamento validados com áudio, PKG
  `b1b1fde6b9368fa4244d0610b50114022ead39796fa63770d8759e3b62ada9a9`.
  720p/1080p e textura GPU/YUV pendentes. Sem push.
- 2026-11-03 - spike Vulkan/RADV validado no hardware usando o SDK/Mesa do
  PS4SX2: `vkCreateInstance` e `vkCreateDevice` OK, GPU detectada como
  `AMD Liverpool (PS4) (RADV BONAIRE)`, imagem linear 1920x1080 (pitch 7680),
  memória GPU host-visible, `sceVideoOutRegisterBuffers` e três flips com clear
  vermelho/verde/azul. O probe é local (`.build/vkprobe`, não versionado).
  A integração com o runtime React ainda precisa do toolchain PS4SX2
  (libc++21/Mesa) ou de um PRX Vulkan; 720p/1080p, 4K/HDR e UI na GPU continuam
  pendentes. Sem push.
- 2026-11-03 - runtime React completo no toolchain PS4SX2 (clang-21/libc++21 +
  Mesa/RADV): UI 1080p60 apresentada como textura Vulkan em dois buffers de
  scanout alternados (PKG
  `8007799c6575f21768a048053f6d3a8b9930d37edb0afd825a4e71f6fe4fcdc7`), vídeo
  NV12 composto como texturas Y/UV no fragment shader sem conversão na CPU, com
  cores, posição, pause/seek/stop e saída limpa validados. O mesmo build linka
  HarfBuzz + SheenBidi + `hb-raster` com libc++21 e renderizou japonês, chinês e
  árabe no hardware (PKG de teste
  `d19d02c43e7394bd31415ffffeb4b6d3a9f35df4ccfc966439c48c640a94a87b`, com
  fontes Noto completas). Commit local `7663e06`; sem push. O projeto CMake
  Vulkan ainda é um spike em `.build`; falta torná-lo um alvo opt-in do repo e
  substituir as fontes completas por subsets no empacotamento.
- 2026-11-03 - sample `text` validado no hardware (PKG
  `750d1ab986fdcafb96f633323057dff2d399693f2dd4f7d73a4d75e3b1182b2e`): japonês,
  chinês, árabe, hindi e bengali com subsets Noto (~180 KB no total), fontes
  copiadas automaticamente de `fonts/` para `/app0/fonts`, parágrafo longo
  completo e ordem RTL conferida pelos runs (`267-275` antes de `197-209`).
  O limite de 255 bytes por nó cortava o parágrafo; o patch
  `patches/embeddedReact-text-size.patch` eleva para 1023 bytes (768 bytes por
  nó, ~0,4 MB com `ER_MAX_NODES=512`). Sem push.
- 2026-11-03 - música HTTP validada no hardware (sample music, PKG
  `87dca4b5aeb7bb657c91a6ec3bf9039de3a434aaa58a1bbb236d9daa736f1c39`): o
  walkthrough `Music.play(url)` usa `mp3dec_decode_frame` sobre um ring buffer
  de 1 MiB preenchido por curl, sem baixar a faixa inteira; log
  `music: http://192.168.0.8:8010/music.mp3 44100 Hz 2 ch (stream)` e o servidor
  registrou os GETs. `seek` fica restrito ao arquivo local, pois
  `mp3dec_ex_open_cb` exige fonte seekable e varre o stream. Metadados ID3
  (`SoundHelix`) e playlist com Previous/Next/auto-avanço no sample. Crossfade,
  OGG/Opus, capa e BGM do sistema pendentes. Sem push.
- 2026-11-03 - crossfade de música validado no hardware (sample music, PKG
  `1781637946dfa9f96eda37b7e2b3425d13dd01509386fa4f2921ac6cdb675c9c`): o mixer
  passou a ter três streams (música A = 0, áudio de vídeo = 1, música B = 2),
  cada faixa usa seu deck e a anterior cai em 1,5 s enquanto a nova sobe; a
  playlist dispara a próxima antes do fim do arquivo local. O log mostrou duas
  trocas locais e a entrada do HTTP sem erro; o abort do GET antigo deixou de
  ser registrado como falha. Duração do HTTP estimada por Content-Length +
  bitrate. OGG/Opus, capa e BGM do sistema pendentes. Sem push.
- 2026-11-03 - OGG Vorbis local e HTTP validados no hardware (sample music,
  PKG `d94b79f3a035e794810154b86c2fa56f6e34949d62985d0cc3ff7ecddc111747`): o
  `stb_vorbis` (domínio público, vendorizado) decodifica arquivo local e
  pushdata sobre o mesmo ring HTTP; log
  `music: /data/pkg/music.ogg 44100 Hz 2 ch, 16436736 frames` e
  `music: http://.../music.ogg 44100 Hz 2 ch (stream)`, com GET no servidor,
  seek local e crossfade. A duração do MP3 local foi corrigida (estava contando
  samples intercalados; agora ~6:12 como o OGG). Opus, capa e BGM pendentes.
  Sem push.
- 2026-11-03 - capa de álbum MP3 validada no hardware (sample music, PKG
  `b1e8db1fe204c6650a9caf5870112368b59af5280956ba871cf1a032c342ad6d`): o frame
  APIC do ID3 é extraído, decodificado com stb_image, premultiplicado e
  registrado no engine como `@music:<id>`; o `<Image source={cover}>` do sample
  exibe a capa 220×220. O `music.mp3` de teste ganhou TIT2/TALB e uma capa
  320×320. Capa em OGG, Opus e BGM do sistema pendentes. Sem push.
- 2026-11-03 - capa OGG validada no hardware (sample music, PKG
  `aa9b91fb09ce075b6af55eeab2f49fbfe2f8b2167b7ee249066d1a64d7e7ba18`): o
  `METADATA_BLOCK_PICTURE` dos comentários Vorbis é decodificado de base64,
  interpretado como FLAC Picture e registrado no engine; `COVERART` serve de
  fallback e as chaves são comparadas sem diferenciar maiúsculas/minúsculas
  (o mutagen grava em minúsculas). O `music.ogg` de teste ganhou capa e
  TITLE/ARTIST/ALBUM; local e HTTP exibem a mesma arte do MP3. Opus e BGM do
  sistema pendentes. Sem push.
- 2026-11-03 - samples `gallery` e `list` validados no hardware: gallery com 9
  PNGs de 640×360 empacotados (PKG
  `2af34767fc7315e878e35ac8d2d760bbf4847157d879e916563a3cfc2f49d2aa`,
  CUSA99081), grid de 3 colunas, foco e `ScrollView`; list com `VirtualList`
  de 1000 linhas (PKG
  `7686c3d0eb2676eedd9ed427d4700bf6923a94817f196c3389dbfe08ff658828`,
  CUSA99082), janela de montagem e scroll pelo foco. Ambos a 59,9 fps. Sem push.
```

## Changelog do plano

- 2026-11-02 — plano inicial; baseline marcado; fases 1–6 e spikes definidos.
