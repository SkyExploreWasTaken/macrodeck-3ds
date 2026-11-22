# Third-party notices

Macro Deck for 3DS includes or links the components below. Each keeps its own license; the rest of the project is
GPL-3.0 (see [LICENSE](LICENSE)). All of these licenses are compatible with the GPL-3.0. The `.3dsx` and `.cia` builds contain the compiled code of every component in the first two
tables.

## Vendored in this repository

| Component | Where | License | Text |
| --- | --- | --- | --- |
| [cJSON](https://github.com/DaveGamble/cJSON) 1.7.18, © 2009-2017 Dave Gamble and cJSON contributors | `lib/cjson/` | MIT | [lib/cjson/LICENSE](lib/cjson/LICENSE) |
| [stb_image](https://github.com/nothings/stb) 2.30, Sean Barrett | `lib/stb/` | Public domain (or MIT, your choice) | end of [stb_image.h](lib/stb/stb_image.h) |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) 1.5.0 (decoder only), © 2010 Google Inc. | `lib/libwebp/` | BSD-3-Clause, plus Google's patent grant | [COPYING](lib/libwebp/COPYING), [PATENTS](lib/libwebp/PATENTS) |
| Mozilla CA certificate bundle (via [certifi](https://github.com/certifi/python-certifi)) | `data/cacert.bin` | MPL-2.0 | [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt) |

## Linked from devkitPro when building

| Component | License | Text |
| --- | --- | --- |
| [libctru](https://github.com/devkitPro/libctru) 2.7, © libctru contributors | Zlib | [licenses/Zlib.txt](licenses/Zlib.txt) |
| [citro2d](https://github.com/devkitPro/citro2d) 1.7 and [citro3d](https://github.com/devkitPro/citro3d) 1.7, © fincs and contributors | Zlib | [licenses/Zlib.txt](licenses/Zlib.txt) |
| [curl](https://curl.se/) 8.4, © 1996-2023 Daniel Stenberg and contributors | curl (MIT-style) | [licenses/curl.txt](licenses/curl.txt) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 2.28, © The Mbed TLS Contributors | Apache-2.0 | [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| [zlib](https://zlib.net/) 1.3.1, © 1995-2024 Jean-loup Gailly and Mark Adler | Zlib | [licenses/Zlib.txt](licenses/Zlib.txt) |
| newlib C library and GCC runtime from devkitARM | BSD-style / GCC Runtime Library Exception | shipped with devkitARM |

## Build-time only (nothing of these ends up in the app)

- [Inter](https://rsms.me/inter/) typeface, © The Inter Project Authors, SIL Open Font License 1.1
  ([licenses/OFL-1.1.txt](licenses/OFL-1.1.txt)): `tools/make_art.py` uses it to draw the text in `meta/icon.png`
  and `meta/banner.png`. The OFL allows that; the font file itself is not included.
- makerom ([Project_CTR](https://github.com/3DSGuy/Project_CTR), MIT) and
  [bannertool](https://github.com/Epicpkmn11/bannertool) (MIT) build the `.cia` in GitHub Actions.

## Trademarks

Macro Deck is a project of its own authors, and Nintendo 3DS is a trademark of Nintendo. This client is an
independent, unofficial project that talks to Macro Deck over its public network API. It contains no Macro Deck
or Nintendo code, and is not affiliated with or endorsed by either.
