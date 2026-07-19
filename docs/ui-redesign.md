# Inventatory UI Redesign v2

This document is the source of truth for the Inventatory terminal interface. Inventatory is a
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
Inventatory Scan Setup page.

The retired phone/web scanner is not part of the v2 interface. The physical
Inventatory Scan R1 remains supported through the desktop application's authenticated
device service; UI copy calls this the R1 service rather than a generic bridge.

The minimum supported terminal is 100 by 30 cells. Smaller terminals show a
resize notice instead of a clipped interface.

Fresh-install setup is the sole shell exception. It uses a full-window,
command-style terminal flow with no navigation, operational status, search, or
action sheet. It configures the data folder, optional background mode, and an
optional physical Inventatory Scan R1 only; printer integration
remain later Settings tasks. The normal Scan R1 setup page remains available
after onboarding.

## Graphite / Turquoise palette

| Role | RGB / hex | Use |
|---|---|---|
| Canvas | `12,14,15` / `#0C0E0F` | Application background |
| Surface | `19,22,25` / `#131619` | Tables and content |
| Raised | `28,32,36` / `#1C2024` | Controls and overlays |
| Hover | `37,43,48` / `#252B30` | Mouse hover |
| Selection | `18,61,64` / `#123D40` | Current row/cell/field |
| Divider | `51,57,61` / `#33393D` | Necessary separators |
| Primary text | `241,243,243` / `#F1F3F3` | Important content |
| Secondary text | `198,202,203` / `#C6CACB` | Labels |
| Muted text | `141,148,151` / `#8D9497` | Hints/inactive data |
| Interactive | `73,212,203` / `#49D4CB` | Actions and brand cues |
| Focus | `140,237,230` / `#8CEDE6` | Active focus |
| Link | `114,199,238` / `#72C7EE` | External links |
| Success | `117,199,147` / `#75C793` | Ready/completed |
| Warning | `216,160,75` / `#D8A04B` | Attention/low stock |
| Danger | `220,116,107` / `#DC746B` | Error/destructive/out |

Turquoise means interactive, active, or focused. Ordinary headings are neutral.
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

An operational overview with a compact operations row, a horizontal health
strip, a compact auto-rotating stock-status list beside recent activity, and
system state. It is not a grid of setup cards. Out-of-stock part text in the
Home stock-status list flashes its red highlight so urgent shortages stand out.

### Stock

One split workspace owns browsing, complete details, links, quantity changes,
printing, creation, and non-modal editing. Detail information is ordered by
decision value: name and description, electrical/rack/quantity essentials,
full electrical parameters, inventory identity, references, then notes. The
inventory list remains visible while editing. Save commits the entire working
copy; Escape cancels it.

### Racks

Rack list, 5x5 grid, and slot detail share one surface with subtle dividers.
Every occupied slot wraps its part name and shows a color-coded quantity
highlight. The slot label is centered at the top of its cell. Rows and cells
are clickable. Place/Move is the primary control; slot controls are anchored
at the bottom of the detail panel and administrative operations remain in
Actions.

### Import

The empty state starts file selection. Imported rows are reviewed beside their
details, corrected inline, and accepted or skipped. IECD enrichment runs locally
for accepted rows and the completion summary reports created, merged, and skipped counts.

### Settings

General/Data, Printer, Quick Labels, and Inventatory Scan live in one category/detail
workspace. Ordinary edits are staged and use Save/Cancel. Refresh, Test, Copy,
Regenerate, and Clear are operational actions. IECD snapshot state and update
controls live under General/Data; there are no database credentials.

## Reusable UI rules

- Prefer whitespace and alignment over borders.
- Use a divider only between major functional regions.
- Controls are compact text labels on a raised surface, not decorative cards.
- Tables use fixed columns and right-aligned numbers.
- Selection combines a dark turquoise background, focus-colored text, and a marker.
- Messages never stack; the newest message replaces the previous one.
- Confirmations use a raised surface, explicit danger text, and an unlock step
  where accidental activation would be costly.
