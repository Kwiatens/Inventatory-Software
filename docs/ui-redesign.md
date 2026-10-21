# Inventatory UI Redesign

This document describes the design intent of the Inventatory terminal interface.
Where it differs from [ui-style-guide.md](ui-style-guide.md), the style guide is binding.
Inventatory is a
compact electronics-inventory tool: text first, fast under the keyboard, and
fully approachable with a mouse. Styling is used only to communicate structure,
focus, interaction, or state.

## Application shell

Every workspace uses the same four regions:

1. Persistent navigation and live system state
2. One optional context/search line
3. Flexible workspace content
4. One transient message line

The navigation is `1 Stock`, `2 Racks`, `3 Import`, `4 Projects`, `5 History`, `6 Settings`.
`Actions` is always visible at the right of the header and opens the contextual
action sheet. `Space` opens the same sheet from the keyboard.
There is no persistent action wall and no separate Detail page. Scan R1 setup
and DigiKey setup are guided pages opened from Settings.

The retired phone/web scanner is not part of the Inventatory interface. The physical
Inventatory Scan R1 remains supported through the desktop application's authenticated
device service; UI copy calls this the R1 service rather than a generic bridge.

The minimum supported terminal is 100 by 30 cells. Smaller terminals show a
resize notice instead of a clipped interface.

Fresh-install setup is the sole shell exception. It uses a full-window,
centered terminal flow with the Inventatory wordmark and no navigation,
operational status, search, or action sheet. It confirms the data folder, offers
background mode and physical Inventatory Scan R1 setup, and leaves printer and
vendor configuration for Settings. The Scan R1 setup page remains
available from Settings after onboarding.

Setup wizards show only task-specific instructions, inputs, selections, and
actions. They do not add command-style headers, step counters, progress strips,
or titled decorative chrome around the content.

## Neutral graphite / Petrol-cyan palette

These are the defaults in `src/app/settings/AppSettings.h`. Users can change them in
Settings. The status colours and their rules are in the style guide.

| Role | RGB / hex | Use |
|---|---|---|
| Canvas | `13,16,16` / `#0D1010` | Application background |
| Surface | `20,25,24` / `#141918` | Tables and content |
| Raised | `29,36,34` / `#1D2422` | Controls and overlays |
| Hover | `39,49,47` / `#27312F` | Mouse hover |
| Selection | `44,68,64` / `#2C4440` | Current row/cell/field |
| Divider | `56,69,67` / `#384543` | Necessary separators |
| Primary text | `229,237,236` / `#E5EDEC` | Important content |
| Secondary text | `191,202,201` / `#BFCAC9` | Labels |
| Muted text | `140,150,144` / `#8C9690` | Hints/inactive data |
| Interactive | `88,185,176` / `#58B9B0` | Actions, active status cues, and brand cues |
| Focus | `185,231,221` / `#B9E7DD` | Active focus |
| Link | `143,203,197` / `#8FCBC5` | External links and progress |
| Success | `160,208,192` / `#A0D0C0` | Ready/completed |
| Warning | `219,205,169` / `#DBCDA9` | Attention/low stock |
| Warning surface | `42,38,28` / `#2A261C` | Low-stock highlights |
| Danger | `204,139,164` / `#CC8BA4` | Error/destructive/out |
| Danger surface | `50,32,39` / `#322027` | Out-of-stock highlights |
| Active surface | `49,90,85` / `#315A55` | Scanner movement/build focus |

Petrol-cyan means interactive, active, focused, linked, or progressing. Ordinary
headings use the primary text colour; graphite surfaces provide structure without making the
whole interface blue. Success, warning, and danger colours are reserved for real state and are
always accompanied by a word. Normal rows share one surface; hover and
selection provide the only background changes.

Truecolor is the reference rendering. Text keeps all states understandable without colour.

The normal `.\run.ps1` launch uses the classic Windows Console host
(`conhost.exe`). JetBrains Mono is the recommended font for the optional Windows
Terminal presentation. The repository includes `docs/windows-terminal-profile.json`
as a profile snippet; it is intentionally not installed or merged into the user's
terminal settings automatically. Developers can launch with
`.\run.ps1 -WindowsTerminal` after adding that profile as `Inventatory`.

## Input model

- `1`-`6`: switch workspace outside text entry
- `Tab` / `Shift+Tab`: move focus
- `Enter`: activate the focused control or current row
- arrows: navigate the active list, grid, category, or field
- `/`: stock search
- `Space`: open the action sheet
- `Esc`: cancel, close, or move back one interaction level
- `q`: quit when not typing

Mouse clicks select navigation, rows, cells, fields, links, categories, buttons,
and action entries. The wheel scrolls or moves the active list. Destructive
actions require an explicit command and confirmation; double-click is never
required.

## Workspace contracts

### Stock

One split workspace owns browsing, complete details, links, quantity changes,
printing, creation, and non-modal editing. Detail information is ordered by
decision value: name and description, electrical/rack/quantity essentials,
full electrical parameters, inventory identity, references, then notes. The
inventory list remains visible while editing. Save commits the entire working
copy; Escape cancels it.

The detail panel header and its action row sit outside the scrolling body, so
New/Edit/-/+/Print stay reachable at the bottom of the panel however long the
selected part is. Quantity and rack are a compact strip directly under the
summary, next to the buttons that change them, and are not repeated in the
identity block where the list columns already show them. Vendor and catalogue
identifiers sit in a collapsed `Details` disclosure whose heading states how many
fields are hidden. Fields with no value are omitted, including vendor
placeholders such as a lone dash or "N/A"; the package value appears once in the
passive summary line rather than again in the parameter list.

Name sorting orders by category then part name, so the list renders one
contiguous run per category behind a single group header on a raised grey band,
and the part column takes the width a repeated category column would waste. Every
list row - item, group header, and group spacer - carries the part/quantity
separator at the same column so that vertical rule runs unbroken down the panel.
Quantity sorting interleaves categories, so it falls back to a per-row
category column sized to its longest value and placed beside the part name, with
the leftover width as one gutter before the right-anchored quantity. No layout
pads a category cell out to absorb slack.

### Racks

Rack list, 5x5 grid, and slot detail share one surface with subtle dividers.
Every occupied slot wraps its part name and shows a color-coded quantity
highlight. The slot label is centered at the top of its cell. Rows and cells
are clickable. Move is the primary control; slot controls are anchored
at the bottom of the detail panel and administrative operations remain in
Actions.

### Import

The empty state starts file selection. Imported rows are reviewed beside their
details, corrected inline, and accepted or skipped. DigiKey synchronization is
offered after review with an explicit completion choice.

### Settings

General, Printer, Quick Labels, Inventatory Scan, and DigiKey live in one category/detail
workspace. Ordinary edits are staged and use Save/Cancel. Refresh, Test, Copy,
Regenerate, and Clear are operational actions. DigiKey secrets are stored in
the operating system credential store (Windows Credential Manager or Linux Secret Service), never in `settings.conf`.

Each panel carries only settings and state: no explanatory hint copy, and no
navigation button that duplicates a category already in the sidebar. An
unconfigured integration or device shows only a filled cyan `Begin setup`
button; setup guidance lives in its wizard. Once setup is accepted, the panel
restores its settings and state rows. Every other control is an ordinary raised
text button. Panels do not restate diagnostics that the device reports
elsewhere, and values derived from an absent device are omitted rather than
shown as zeros.

## Reusable UI rules

- Prefer whitespace and alignment over borders.
- Use a divider only between major functional regions.
- Controls are compact text labels on a raised surface, not decorative cards.
- Tables use fixed columns and right-aligned numbers.
- Selection combines a dark selection background and focus-colored text.
- Messages never stack; the newest message replaces the previous one.
- Confirmations use a raised surface, explicit danger text, and an unlock step
  where accidental activation would be costly.
