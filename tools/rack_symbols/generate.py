#!/usr/bin/env python3
"""Render the rack label symbols into a generated C++ source file.

See README.md. Needs Pillow and Chrome/Chromium (headless) on PATH.
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUTPUT = os.path.join(REPO, "src", "label_printer", "symbols", "RackSymbolsData.generated.cpp")
SUPERSAMPLE = 8
TILE = 150


def chrome():
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("generate.py: Google Chrome or Chromium is required on PATH")


def read_svg(name):
    with open(os.path.join(HERE, "svg", name + ".svg"), encoding="utf-8") as handle:
        return handle.read().strip()


def with_stroke(svg, width):
    out, at = [], 0
    marker = 'stroke-width="'
    while True:
        start = svg.find(marker, at)
        if start < 0:
            out.append(svg[at:])
            return "".join(out)
        end = svg.index('"', start + len(marker))
        out.append(svg[at:start])
        out.append('%s%s"' % (marker, ("%.4f" % width).rstrip("0").rstrip(".")))
        at = end + 1


def shoot(svg, width_px, height_px, left_px, top_px, size_px):
    """Render the SVG tile at size_px square placed at (left, top) on a white page."""
    svg = svg.replace("<svg ", '<svg style="position:absolute;left:%.3fpx;top:%.3fpx;width:%dpx;height:%dpx" ' % (
        left_px, top_px, size_px, size_px), 1)
    html = ('<!doctype html><meta charset="utf-8"><style>html,body{margin:0;background:#fff;overflow:hidden}'
            'svg{shape-rendering:geometricPrecision}</style><body>%s</body>' % svg)
    with tempfile.TemporaryDirectory() as tmp:
        page, png = os.path.join(tmp, "p.html"), os.path.join(tmp, "p.png")
        with open(page, "w", encoding="utf-8") as handle:
            handle.write(html)
        subprocess.run([chrome(), "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                        "--force-device-scale-factor=1", "--window-size=%d,%d" % (width_px, height_px),
                        "--screenshot=" + png, "file://" + page],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return Image.open(png).convert("L").copy()


def geometry(svg):
    """Bounding box of the drawn geometry in tile units, without the half stroke."""
    side = TILE * SUPERSAMPLE
    image = shoot(svg, side, side, 0, 0, side)
    box = image.point(lambda v: 255 if v < 128 else 0).getbbox()
    stroke = 5.0  # all source tiles are drawn at stroke-width 5
    x0, y0, x1, y1 = [v / SUPERSAMPLE for v in box]
    return x0 + stroke / 2, y0 + stroke / 2, x1 - x0 - stroke, y1 - y0 - stroke


def render(name, box, anchor_x=None, max_height=None):
    if max_height is not None:
        box = dict(box, height=max_height)
    svg = read_svg(name)
    gx, gy, gw, gh = geometry(svg)
    if anchor_x is not None:
        # Centre on a feature (a transistor's circle) instead of the bounding box, which also holds long leads.
        half = max(anchor_x - gx, gx + gw - anchor_x)
        gx, gw = anchor_x - half, 2 * half
    stroke = box["stroke"]
    k = min((box["width"] - stroke) / gw, (box["height"] - stroke) / gh)
    # Round to the nearest dot so the bitmap hugs its drawing: an empty trailing column would push the symbol off
    # the centre of the label.
    width = int(round(gw * k + stroke))
    height = int(round(gh * k + stroke))
    # Put the horizontal axis (tile y = 75, where nearly every lead sits) on a dot boundary so an even stroke
    # covers whole rows instead of straddling two.
    top_dots = stroke / 2 - gy * k
    axis = (75 - gy) * k + stroke / 2
    top_dots -= axis - round(axis)
    left_dots = stroke / 2 - gx * k
    image = shoot(with_stroke(svg, stroke / k), (width + 1) * SUPERSAMPLE, (height + 1) * SUPERSAMPLE,
                  left_dots * SUPERSAMPLE, top_dots * SUPERSAMPLE, int(round(TILE * k * SUPERSAMPLE)))
    small = image.resize((width + 1, height + 1), Image.BOX).crop((0, 0, width, height))
    bytes_per_row = (width + 7) // 8
    data = bytearray()
    for y in range(height):
        row = 0
        for x in range(bytes_per_row * 8):
            row <<= 1
            if x < width and 255 - small.getpixel((x, y)) >= 128:
                row |= 1
            if x % 8 == 7:
                data.append(row)
                row = 0
    return width, height, bytes_per_row, bytes(data)


def identifier(name):
    return "k" + "".join(part.capitalize() for part in name.replace("_", "-").split("-"))


def generate():
    with open(os.path.join(HERE, "symbols.json"), encoding="utf-8") as handle:
        manifest = json.load(handle)
    box = manifest["box"]
    names = []
    anchors = {}
    max_heights = {}
    for entry in manifest["symbols"]:
        for standard in ("eu", "us"):
            if entry[standard] not in names:
                names.append(entry[standard])
            if "anchorX" in entry:
                anchors[entry[standard]] = entry["anchorX"]
            if "maxHeight" in entry:
                max_heights[entry[standard]] = entry["maxHeight"]
    lines = [
        "// Generated by tools/rack_symbols/generate.py from the SVG files in tools/rack_symbols/svg.",
        "// Do not edit: change the artwork or symbols.json and regenerate. Provenance and licences are in",
        "// tools/rack_symbols/README.md and THIRD_PARTY_NOTICES.md.",
        "",
        '#include "label_printer/symbols/RackSymbols.h"',
        "",
        "namespace inventatory {",
        "namespace {",
        "",
        "// 1 bit per dot, rows padded to whole bytes, most significant bit first, 1 = black.",
    ]
    for name in names:
        width, height, bytes_per_row, data = render(name, box, anchors.get(name), max_heights.get(name))
        lines.append("// %s: %d x %d dots" % (name, width, height))
        lines.append("constexpr unsigned char %sData[] = {" % identifier(name))
        for at in range(0, len(data), 16):
            lines.append("    " + ", ".join("0x%02X" % b for b in data[at:at + 16]) + ",")
        lines.append("};")
        lines.append("constexpr RackSymbolBitmap %s = {%d, %d, %d, %sData};" % (
            identifier(name), width, height, bytes_per_row, identifier(name)))
        lines.append("")
    lines += ["}  // namespace", "", "const RackSymbolSet kRackSymbolSets[] = {"]
    for entry in manifest["symbols"]:
        lines.append('    {"%s", &%s, &%s},' % (entry["key"], identifier(entry["eu"]), identifier(entry["us"])))
    lines += ["};", "", "const size_t kRackSymbolSetCount = sizeof(kRackSymbolSets) / sizeof(kRackSymbolSets[0]);", "",
              "}  // namespace inventatory", ""]
    return "\n".join(lines)


def main():
    text = generate()
    if "--check" in sys.argv:
        with open(OUTPUT, encoding="utf-8") as handle:
            if handle.read() != text:
                sys.exit("generate.py: %s is out of date; run tools/rack_symbols/generate.py" % OUTPUT)
        print("up to date")
        return
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote", OUTPUT)


if __name__ == "__main__":
    main()
