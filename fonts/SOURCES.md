# Substitute fonts

When a PDF names a font without embedding it, PDF/A needs one that is. The converter embeds a
font from here, cut down to the glyphs the document uses, and keeps the document's own widths,
so no line moves.

- **Twins:** fonts with the same width for every character as the one they stand in for. The
  page looks as the font was meant to.
- **Close fonts:** for the rest, the free font whose widths come closest to the document's, in
  the same style (sans, serif, monospaced; bold, italic). Letters sit where the document puts
  them, as every reader already draws a missing font; their shapes differ.

| Files             | Stands in for                                    | Source                                                    | Licence                                    |
| ----------------- | ------------------------------------------------ | --------------------------------------------------------- | ------------------------------------------ |
| `Arimo-*`         | Arial, Helvetica (twin)                          | googlefonts/arimo `fonts/ttf` (1.33)                      | SIL OFL 1.1, `licenses/Arimo-OFL.txt`      |
| `Tinos-*`         | Times New Roman, Times (twin)                    | google/fonts `ofl/tinos`                                  | SIL OFL 1.1, `licenses/Tinos-OFL.txt`      |
| `Cousine-*`       | Courier New, Courier (twin)                      | google/fonts `ofl/cousine`                                | SIL OFL 1.1, `licenses/Cousine-OFL.txt`    |
| `Carlito-*`       | Calibri (twin)                                   | google/fonts `ofl/carlito` at e7db6ca603fd                | SIL OFL 1.1, `licenses/Carlito-OFL.txt`    |
| `Caladea-*`       | Cambria (twin)                                   | google/fonts `ofl/caladea` at e7db6ca603fd                | SIL OFL 1.1, `licenses/Caladea-OFL.txt`    |
| `DejaVuSans*`     | close sans; Symbol and ZapfDingbats characters   | dejavu-fonts 2.37 release (`dejavu-fonts-ttf-2.37.tar.bz2`, SHA-256 fa9ca4d13871…e17d7) | Bitstream Vera licence, `licenses/DejaVu-LICENSE.txt` |
| `NotoSans-*`      | close sans                                       | notofonts/notofonts.github.io `fonts/NotoSans/unhinted/ttf` | SIL OFL 1.1, `licenses/Noto-OFL.txt`     |
| `NotoSerif-*`     | close serif                                      | notofonts/notofonts.github.io `fonts/NotoSerif/unhinted/ttf` | SIL OFL 1.1, `licenses/Noto-OFL.txt`    |
| `DroidSansFallbackFull.ttf` | close font for Chinese, Japanese and Korean (the one Ghostscript uses) | AOSP platform/frameworks/base `data/fonts` at tag android-8.1.0_r1 | Apache-2.0, `licenses/DroidSansFallback-NOTICE.txt` |
| `cmaps/Uni*-UCS2-[HV]` | the predefined Unicode CMaps (code → CID) of Adobe-Japan1, -CNS1, -GB1, -Korea1, for fonts that name them | adobe-type-tools/cmap-resources at f5cf3bca7fdf | BSD-3-Clause, `licenses/cmap-resources-LICENSE.md` |

Arimo, Tinos, Cousine and Noto were downloaded on 2026-09-19, and Carlito, Caladea, DejaVu Sans,
Droid Sans Fallback and the CMaps on 2026-10-24, from the sources above.
`SHA256SUMS` lists every file.
