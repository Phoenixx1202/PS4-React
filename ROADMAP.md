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
- [ ] validar shaping Indic no hardware;
- [ ] documentar fontes empacotadas versus carregadas.

### 1.7 Multi-controle e multi-usuário

- [ ] enumerar usuários e abrir um pad por usuário;
- [ ] `Controller`/`useGamepad(playerIndex)` e troca de jogador;
- [ ] eventos de login/logout;
- [ ] teste com dois controles.

### 1.8 Rede avançada

- [ ] WebSocket cliente (texto/binário, ping/pong, reconexão): cliente
  texto `ws://`/`wss://` e ping/pong implementados; binário de saída e
  reconexão automática pendentes; validação de hardware pendente;
- [x] SSE/streaming HTTP para respostas longas (cliente `Sse` validado com
  servidor local; fila limitada a 256 eventos);
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
- [ ] `text` (CJK/árabe/Indic)
- [x] `device` (DeviceInfo/Users/Notifications) — validado, incluindo
  temperatura via `libkernel_sys` e memória flexível disponível;
- [ ] `gallery` (imagens + ScrollView)
- [ ] `list` (VirtualList com centenas de linhas)

## Fase 2 — Mídia (P1)

### 2.1 Música e streaming

- [ ] decoders MP3/OGG/Opus (minimp3/stb_vorbis/Opus);
- [ ] `Music.play(file|url)`, pause/resume/seek/volume;
- [ ] playlists, crossfade e metadados (título/artista/capa);
- [ ] usar os dois decks e o `stream_ring` existentes no mixer;
- [ ] BGM do sistema (`sceMusicPlayerService`) quando disponível;
- [ ] sample `music`.

**Aceite:** tocar MP3 local e stream HTTP com crossfade, sem underrun e com
CPU baixa.

### 2.2 Vídeo

- [ ] spike `sceAvPlayer` (formatos, áudio, legendas, faixas);
- [ ] `<Video>` e API `AvPlayer` para arquivo local e URL;
- [ ] sincronização áudio/vídeo e controle (play/pause/seek/stop);
- [ ] caminho de textura (GPU) e fallback software em resolução limitada;
- [ ] sample `video`.

**Aceite:** MP4 H.264 1080p com áudio sincronizado, seek e sem leak.

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

- [ ] spike de backend: OpenGL ES/Piglet versus GNM (PS4);
- [ ] abstração `Renderer` com implementação software e GPU;
- [ ] atlas de textura, batching, scissor, blend e shaders para a UI 2D;
- [ ] textura de vídeo/imagem e upload assíncrono;
- [ ] manter o software como fallback e para CI;
- [ ] comparar fps, memória e qualidade nos samples.

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
```

## Changelog do plano

- 2026-11-02 — plano inicial; baseline marcado; fases 1–6 e spikes definidos.
