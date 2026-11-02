# Licenças de terceiros do port PS4

| Componente | Licença | Onde |
|---|---|---|
| HarfBuzz 14.6.0 | Old MIT (`harfbuzz-COPYING`) | compilado do rootfs PacBrew para `.build/harfbuzz` |
| SheenBidi 3.0.0 | Apache-2.0 (`sheenbidi-LICENSE`) | `native/ps4/third_party/sheenbidi/` |
| stb_image 2.30 | Public domain / MIT | `native/ps4/third_party/stb_image.h` (texto no topo do header) |
| CA bundle | Mozilla CA list | `assets/ca-bundle.crt` |
| Noto Sans JP/SC/Árabe | SIL OFL 1.1 | opcional, empacotado pelo app em `/app0/fonts` |

O restante do framework mantém as licenças e atribuições do projeto original; veja
`../NOTICE` e `../LICENSE-ATTRIBUTION`.
