# Fontes e texto

O runtime tem dois caminhos de texto:

1. **Fontes baked** (Inter, incluída pelo app): o bundler gera bitmaps por
   tamanho e o compositor desenha sem custo de shaping. Cobre latim e os
   símbolos que o app declarar em `assets.config.js`.
2. **Text shaper em runtime** (`native/ps4/shared/text_shaper.cpp`): HarfBuzz
   para shaping, SheenBidi para a ordem bidirecional e `hb-raster` para
   rasterizar sob demanda. Cobre CJK, árabe, hebraico, devanágari, bengali e
   outros scripts que as fontes baked não desenham. Os glifos ficam num cache
   limitado; as fontes são carregadas na primeira vez que um script aparece.

## Fontes empacotadas

O host chama `text_shaper::install("/app0/fonts", ...)` e o shaper procura estes
nomes exatos:

```text
NotoSansJP-Regular.otf    japonês (kana e han com preferência japonesa)
NotoSansSC-Regular.otf    chinês (han)
NotoSansDevanagari-Regular.ttf
NotoSansBengali-Regular.ttf
NotoSansArabic-Regular.ttf
```

Qualquer arquivo pode faltar; o script correspondente fica indisponível e o
shaper registra a falha no log. Coloque os arquivos em `fonts/` na raiz do app
(ao lado de `sce_sys/`), que é montada como `/app0/fonts` no console. O bundler
copia o app inteiro para `.build/react/app` e o alvo `ps4_react_starter_stage`
copia `fonts/` para `build/app-react/fonts`; o empacotador inclui essa pasta no
PKG, sem passo manual.

As fontes Noto não são versionadas neste repositório: são grandes e têm licença
própria (SIL Open Font License). Para reproduzir o teste de hardware, baixe as
variáveis do Google Fonts e renomeie:

```bash
mkdir -p app/fonts
curl -L -o app/fonts/NotoSansJP-Regular.otf \
  'https://raw.githubusercontent.com/google/fonts/main/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf'
curl -L -o app/fonts/NotoSansSC-Regular.otf \
  'https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf'
```

Para árabe, qualquer fonte com cobertura árabe serve; o teste usou
`Amiri-Regular.ttf` renomeado para `NotoSansArabic-Regular.ttf`.

O arquivo pode ser TTF mesmo com a extensão `.otf`: o HarfBuzz identifica o
formato pelo conteúdo.

## Tamanho e subsets

As variáveis completas de JP + SC somam cerca de 28 MiB, o que é aceitável para
um teste mas caro para produção. Gere subsets com `pyftsubset` (fonttools) antes
de empacotar, preservando os intervalos Unicode dos scripts usados pelo app, por
exemplo:

```bash
pyftsubset NotoSansJP-Regular.otf --output-file=fonts/NotoSansJP-Regular.otf \
  --unicodes='U+3000-30FF,U+4E00-9FFF,U+FF00-FFEF' --layout-features='*'
```

O shaper não depende do nome da família: ele escolhe o arquivo pelo script
predominante do trecho de texto.

## Limite de texto por nó

O engine guarda o texto de cada nó num buffer inline de `ER_TEXT_MAX` bytes. O
valor original de 255 cortava parágrafos longos antes de chegar ao shaper; o
patch `patches/embeddedReact-text-size.patch` eleva o limite para 1023 bytes
(UTF-8, incluindo o terminador). Textos maiores devem ser divididos em mais de
um nó `<Text>`. O custo é de 768 bytes por nó de texto, cerca de 0,4 MB no total
com `ER_MAX_NODES=512`.

## Idioma

`text_shaper::set_language("ja")` faz o han preferir os glifos japoneses; sem a
chamada, han usa `NotoSansSC`. O host ainda não expõe uma opção de configuração
para isso; o padrão atual é chinês para han e japonês para kana.

## Estado validado

- 2026-11-03: japonês, chinês, árabe, hindi e bengali renderizados no hardware
  no sample `text` (build OpenOrbis); o parágrafo longo completa e o árabe
  aparece na ordem RTL correta.
- Indic está implementado e validado no sample; falta apenas testar variantes
  de fonte que o app não empacote.
- Texto latino continua usando as fontes baked; o shaper só entra quando falta
  glifo ou o script exige shaping/reordenação.
