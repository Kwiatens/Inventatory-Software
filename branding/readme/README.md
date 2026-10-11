# README graphics

Sections of the project README that are drawn in the website's pixel style are generated from
plain-text files in [`src/`](src). Edit a text file, re-render, and commit both: the SVGs next to
this file are output, not source.

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
| `src/roadmap.txt` | `roadmap.svg` and `roadmap-light.svg`, the planned features |

## Source format

One `key: value` per line; blank lines and lines starting with `#` are ignored.

```text
title: Plans and future features

row: MCP integration | Let AI assistants work with your inventory
row: Parametric racks | Generate rack models from a few dimensions
```

The title is the section heading (the README has no Markdown heading for it). Each `row` is a
name and a one-line description, separated by `|`. A row too long for the width stops the render
with a message, and characters outside printable ASCII are not in the font.

## Look

The background is transparent, so the section sits on the page like the rest of the README. Text
is the Scan R1 bitmap font drawn pixel by pixel, with the site's hard drop shadow under the title
and its dotted rules between rows. Every size is a whole multiple of one art pixel (2 units), so
the SVG stays crisp. Each source renders twice: the website's colours, and a light variant that
only swaps the ink so the section stays readable on a light page. The README picks one with
`<picture>`.

The fonts are copied from the website repository into `tools/readme_art_data/`. They are U8g2
bitmap fonts (BSD-2-Clause); see the website's notices.

## Adding a section

Create `src/<name>.txt` and render. Reference it from the README with a `<picture>` that points
at `<name>.svg` and `<name>-light.svg`, `width="830"` (the native width) and a descriptive `alt`.
