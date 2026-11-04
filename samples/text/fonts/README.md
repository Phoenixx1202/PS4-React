# Fontes do sample `text`

Estes arquivos são subsets das fontes Noto Sans JP, SC, Arabic, Devanagari e
Bengali, cobrindo apenas os caracteres usados neste sample. São pequenos de
propósito: o app real deve gerar um subset com o texto que usa.

- Copyright: The Noto Project Authors (https://github.com/notofonts)
- Licença: SIL Open Font License 1.1 (veja `OFL.txt`)

Gerados a partir das variáveis do Google Fonts com `pyftsubset`:

```bash
pyftsubset NotoSansJP-Regular.ttf --output-file=NotoSansJP-Regular.otf \
  --text=' こんにちは...' --layout-features='*' --name-IDs=1,2,3,4,6
```

Para trocar o texto do sample, refaça os subsets com os novos caracteres. O
shaper escolhe o arquivo pelo script do trecho, não pelo nome da família, então
os nomes acima precisam ser mantidos.
