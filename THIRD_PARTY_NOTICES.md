# Third-party notices

Inventatory links against [FTXUI](https://github.com/ArthurSonzogni/FTXUI), pinned to commit
`182ef70cd4dc1109d06e66361a9da86445928578`, under the MIT License:

Copyright (c) 2019 Arthur Sonzogni.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

Inventatory statically links SQLite 3.53.4 from the official SQLite amalgamation. SQLite is in the public domain; its source and release information are available at [sqlite.org](https://www.sqlite.org/).

The rack label electrical symbols are rendered from [electronic-symbols](https://github.com/chris-pikul/electronic-symbols) by Chris Pikul under the MIT License. The artwork is used as published, except the transistor, whose three leads are shortened so its circle can be drawn larger:

MIT License

Copyright (c) 2022 Chris Pikul

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The geometry of the IEC fuse symbol follows the fuse in [Electrical Symbols IEC.svg](https://commons.wikimedia.org/wiki/File:Electrical_Symbols_IEC.svg) on Wikimedia Commons, which is dedicated to the public domain under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The IC package, pin header and rack grid icons are original to Inventatory. Sources and the generator are in `tools/rack_symbols/`. The electronic-symbols license text is in `tools/rack_symbols/LICENSE.electronic-symbols.txt`.

The pixel lettering in the README artwork (`branding/readme/*.svg`, drawn by `tools/readme_art.py` from
`tools/readme_art_data/fonts.json`) uses glyph bitmaps from fonts bundled with
[U8g2](https://github.com/olikraus/u8g2), the same fonts the Scan R1 screen uses. The Helvetica glyphs come from the
Adobe BDF fonts HELVR08, HELVB08 and HELVB10, under this notice:

Copyright 1984-1989, 1994 Adobe Systems Incorporated.
Copyright 1988, 1994 Digital Equipment Corporation.

Adobe is a trademark of Adobe Systems Incorporated which may be
registered in certain jurisdictions.
Permission to use these trademarks is hereby granted only in
association with the images described in this file.

Permission to use, copy, modify, distribute and sell this software
and its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notices appear in all
copies and that both those copyright notices and this permission
notice appear in supporting documentation, and that the names of
Adobe Systems and Digital Equipment Corporation not be used in
advertising or publicity pertaining to distribution of the software
without specific, written prior permission.  Adobe Systems and
Digital Equipment Corporation make no representations about the
suitability of this software for any purpose.  It is provided "as
is" without express or implied warranty.

The monospaced glyphs come from ProFont (ProFont Distribution 2.2, as converted by U8g2).

The test fixtures `tests/fixtures/altium-real-*.csv` are Altium Designer BOM exports copied from public
projects, used only to test the importer:

- `altium-real-gbpp.csv`: [marshallh/gbpp](https://github.com/marshallh/gbpp), MIT License, Copyright (c) 2021 marshallh.
- `altium-real-ledsp.csv`: [pms67/LeDSP-Audio-SoM](https://github.com/pms67/LeDSP-Audio-SoM), MIT License,
  Copyright (c) 2022 Philip Salmony.
- `altium-real-dsi-shield.csv`: [twlostow/dsi-shield](https://github.com/twlostow/dsi-shield), LGPL-3.0.
- `altium-real-battman.csv`: [raphaelchang/battman-hardware](https://github.com/raphaelchang/battman-hardware), LGPL-3.0.
