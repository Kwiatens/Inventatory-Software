# README graphics

The pixel-art panels in the project README are generated from plain-text files in [`src/`](src).
Edit a text file, re-render, and commit both: the SVGs next to this file are output, not source.

```sh
python3 tools/readme_art.py            # render everything
python3 tools/readme_art.py --watch    # re-render each time you save a source file
python3 tools/readme_art.py --check    # exit 1 if an SVG is out of date
```

Only Python 3 is needed. When a source file changes on `main`, the `readme-art` workflow
re-renders the SVGs and commits them, so editing a text file on GitHub is enough. On a pull
request the same workflow fails if the committed SVGs do not match their sources.

| Source | Output |
| --- | --- |
| `src/workflow.txt` | `workflow.svg`, how it works |
| `src/roadmap.txt` | `roadmap.svg`, planned features |

## Source format

One `key: value` per line; blank lines and lines starting with `#` are ignored.

```text
type: panel
title: Planned features
intro: Not built yet.
columns: Item | Status          # the two column headings
sprite: filmRackEmpty           # optional: filmRackEmpty, filmRackFull or scannerSmall
sprite_scale: 3                 # optional: whole number, default 2

row: MCP integration | planned  # one line per table row: left text | right value
row: Parametric racks | planned
```

Text wraps by itself and the panel resizes to fit. A row whose text would run into its value
stops the render with a message. Characters outside printable ASCII are not in the font.

## Look

The panels follow the website: the Scan R1 bitmap fonts drawn pixel by pixel, the stepped corner,
the hard drop shadow, dotted rules, the site's flat colour tokens, and the real CAD-rendered
sprites. Every size is a whole multiple of one art pixel (2 units), so the SVG stays crisp.

The fonts and sprites are copied from the website repository into `tools/readme_art_data/`.
Fonts are U8g2 bitmap fonts (BSD-2-Clause); see the website's notices.

## Adding a panel

Create `src/<name>.txt` and render; it becomes `<name>.svg`. Reference it from the README with an
`<img>` tag and `width="760"` (the panel's native width) and a descriptive `alt`.
