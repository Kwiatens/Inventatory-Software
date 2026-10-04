# Rack label symbols

Source artwork and generator for the electrical symbols printed on rack labels. The application never reads
these SVG files: `generate.py` renders them once into 1-bit bitmaps and writes
`src/label_printer/symbols/RackSymbolsData.generated.cpp`, which is committed.

## Provenance

| File prefix | Origin | Licence |
|---|---|---|
| `pikul-` | [chris-pikul/electronic-symbols](https://github.com/chris-pikul/electronic-symbols), unmodified | MIT, see `LICENSE.electronic-symbols.txt` |
| `pikul-modified-` | The same library with a change to the artwork: the transistor's three leads are 12 tile units shorter so the circle can be drawn larger in the same space. The symbol itself is unchanged | MIT, see `LICENSE.electronic-symbols.txt` |
| `commons-` | Geometry of the fuse in [Electrical Symbols IEC.svg](https://commons.wikimedia.org/wiki/File:Electrical_Symbols_IEC.svg) on Wikimedia Commons (a 30 x 10 rectangle crossed by a line), scaled onto the 150 x 150 tile used by the other symbols | CC0 1.0 |
| `inventatory-` | Drawn for this project: no electrical standard defines a symbol for an IC package, a pin header or a custom rack | GPL-3.0-only, like the rest of the repository |

`symbols.json` maps each rack type to its IEC (EU) and ANSI/IEEE (US) artwork. Only the resistor and the fuse
differ between the two standards; the other symbols are the same in both.

## Regenerating

Requires Python 3 with Pillow and Google Chrome or Chromium on `PATH` (used as the SVG renderer).

```sh
python3 tools/rack_symbols/generate.py          # rewrite the generated file
python3 tools/rack_symbols/generate.py --check  # fail if the committed file is out of date
```

Every symbol is fitted into the box in `symbols.json` (dots, 203 dpi) and drawn at one stroke weight, so the
line weight is identical on every rack label. A symbol can also set `anchorX` (centre on a feature instead of its bounding box) or `maxHeight` (a smaller box) in `symbols.json`. The renderer works at 8x and averages down before thresholding.
