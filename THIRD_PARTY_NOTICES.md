# VectorText third-party notices

VectorText's original code, build scripts, written documentation and sample configuration are licensed under **GPL-3.0-only**. See [LICENSE](LICENSE). Third-party components retain the licenses below; the GPL notice does not replace their copyright or permission notices.

## FreeType

This distribution selects the **FreeType License (FTL)** option for FreeType 2.14.3, source revision `0a0221a1347e2f1e07c395263540026e9a0aa7c7`. FreeType is statically linked into VectorText.dll. The upstream sources are unmodified; `third_party/ftmodule.min.h` is VectorText's separate module configuration.

Portions of this software are copyright © 1996–2026 The FreeType Project (https://freetype.org). All rights reserved.

The complete FTL and upstream license-selection notice are in [LICENSES/FreeType-FTL.txt](LICENSES/FreeType-FTL.txt) and [LICENSES/FreeType-license-selection.txt](LICENSES/FreeType-license-selection.txt). The FreeType project explicitly identifies FTL as compatible with GPLv3: [FreeType licenses](https://freetype.org/license.html).

FreeType also contains separately licensed components. Their original notices are reproduced without changing their terms:

| Component | Notice |
| --- | --- |
| Bundled zlib 1.3.1, used by the gzip module; Jean-loup Gailly and Mark Adler | [zlib license](LICENSES/FreeType-zlib.txt) |
| `fthash.c`; Computing Research Labs, New Mexico State University, and Francesco Zappa Nardelli | [MIT-style license](LICENSES/FreeType-fthash-MIT.txt) |
| HarfBuzz-derived declarations and optional integration; Red Hat and Google | [MIT-style license](LICENSES/FreeType-HarfBuzz-MIT.txt) |
| HarfBuzz script declarations; Red Hat and Google | [MIT-style license](LICENSES/FreeType-HarfBuzz-script-MIT.txt) |

No standalone HarfBuzz library is linked by the current CMake build. Its notices are retained for the imported source files in FreeType.

## MinHook and HDE

The unmodified upstream source is stored as the `third_party/minhook` submodule, pinned to revision `8af6b4acae5a9388fd742b56fa79ece89d96f823` from [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook). The static x86 build uses the same source revision as the previous vendored copy.

MinHook is statically linked for presentation hooks. Copyright (C) 2009–2017 Tsuda Kageyu. Its embedded Hacker Disassembler Engine includes copyright (c) 2008–2009 Vyacheslav Patkov. These components use BSD-2-Clause terms; their complete upstream notices, including the HDE notices and disclaimers, are preserved in [LICENSES/MinHook-BSD-2-Clause.txt](LICENSES/MinHook-BSD-2-Clause.txt).

## Fonts and external software

Font paths in VectorText.ini refer to files installed by the user. No Noto or Arial font file is included in this distribution. Font redistribution, if added, must follow the respective font license.

The game, its assets (including the game imagery in preview screenshots), Syringe, Phobos, Ares and cnc-ddraw are external software. This project's license does not grant rights to redistribute them. Local diagnostics read the user's installed files; the distribution does not include their binaries.

## Redistribution

Keep LICENSE, this file and the LICENSES directory with a binary distribution. GPLv3 also requires access to the corresponding source through a method permitted by section 6; shipping the binary directory alone does not satisfy that requirement. The corresponding source includes the VectorText sources and build scripts, the pinned FreeType source and MinHook source used to build the DLL. Include the actual submodule contents when making a source archive, rather than only a Git submodule pointer. Preserve upstream notices and identify any future changes to upstream sources.
