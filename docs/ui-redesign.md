# HIMS UI Redesign v2

This document is the source of truth for the HIMS terminal interface. HIMS is a
compact electronics-inventory tool: text first, fast under the keyboard, and
fully approachable with a mouse. Styling is used only to communicate structure,
focus, interaction, or state.

## Application shell

Every workspace uses the same four regions:

1. Persistent navigation and live system state
2. One optional context/search line
3. Flexible workspace content
4. One transient message line

The navigation is `1 Home`, `2 Stock`, `3 Racks`, `4 Import`, `5 Settings`.
`Actions · Space` is always visible and opens the contextual action sheet.
There is no persistent action wall and no separate Detail, Printer Setup, or
HIMS Scan Setup page.

The retired phone/web scanner is not part of the v2 interface. The physical
HIMS Scan R1 remains supported through the desktop application's authenticated
device service; UI copy calls this the R1 service rather than a generic bridge.

The minimum supported terminal is 100 by 30 cells. Smaller terminals show a
resize notice instead of a clipped interface.

## Instrument Blue palette

| Role | RGB / hex | Use |
|---|---|---|
| Canvas | `16,22,26` / `#10161A` | Application background |
| Surface | `21,29,34` / `#151D22` | Tables and content |
| Raised | `26,36,42` / `#1A242A` | Controls and overlays |
| Hover | `32,44,51` / `#202C33` | Mouse hover |
| Selection | `25,57,74` / `#19394A` | Current row/cell/field |
| Divider | `44,57,64` / `#2C3940` | Necessary separators |
| Primary text | `216,226,231` / `#D8E2E7` | Important content |
| Secondary text | `170,184,191` / `#AAB8BF` | Labels |
| Muted text | `126,141,149` / `#7E8D95` | Hints/inactive data |
| Interactive | `98,169,209` / `#62A9D1` | Links and actions |
| Focus | `138,200,232` / `#8AC8E8` | Active focus |
| Success | `111,190,140` / `#6FBE8C` | Ready/completed |
| Warning | `213,164,79` / `#D5A44F` | Attention/low stock |
| Danger | `219,116,112` / `#DB7470` | Error/destructive/out |

Blue means interactive, active, or focused. Ordinary headings are neutral.
Green, amber, and red are reserved for real state and are always accompanied by
a word or glyph. Normal rows share one surface; hover and selection provide the
only background changes.

Truecolor is the reference rendering. Terminals without truecolor may use their
nearest xterm-256 colors; text and glyphs keep all states understandable.

## Input model

- `1`–`5`: switch workspace outside text entry
- `Tab` / `Shift+Tab`: move focus
- `Enter`: activate the focused control or current row
- arrows: navigate the active list, grid, category, or field
- `/`: stock search
- `Space`: contextual actions
- `Esc`: cancel, close, or move back one interaction level
- `q`: quit when not typing

Mouse clicks select navigation, rows, cells, fields, links, categories, buttons,
and action entries. The wheel scrolls or moves the active list. Destructive
actions require an explicit command and confirmation; double-click is never
required.

## Workspace contracts

### Home

An operational overview: inventory health, attention items, recent activity,
scanner/printer/device state, and direct next steps. It is not a grid of setup
cards.

### Stock

One split workspace owns browsing, complete details, links, quantity changes,
printing, creation, and non-modal editing. The inventory list remains visible
while editing. Save commits the entire working copy; Escape cancels it.

### Racks

Rack list, 5x5 grid, and slot detail share one surface with subtle dividers.
Rows and cells are clickable. Place/Move is the primary control; administrative
operations remain in Actions.

### Import

The empty state starts file selection. Imported rows are reviewed beside their
details, corrected inline, and accepted or skipped. DigiKey synchronization is
offered after review with an explicit completion choice.

### Settings

General/Data, Printer, HIMS Scan, and DigiKey live in one category/detail
workspace. Ordinary edits are staged and use Save/Cancel. Refresh, Test, Copy,
Regenerate, and Clear are operational actions. DigiKey secrets are stored in
Windows Credential Manager, never in `settings.conf`.

## Reusable UI rules

- Prefer whitespace and alignment over borders.
- Use a divider only between major functional regions.
- Controls are compact text labels on a raised surface, not decorative cards.
- Tables use fixed columns and right-aligned numbers.
- Selection combines a blue background, focus-colored text, and a marker.
- Messages never stack; the newest message replaces the previous one.
- Confirmations use a raised surface, explicit danger text, and an unlock step
  where accidental activation would be costly.
