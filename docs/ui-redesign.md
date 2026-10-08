# Inventatory UI Redesign

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

The navigation is `1 Stock`, `2 Racks`, `3 Import`, `4 Projects`, `5 History`, `6 Settings`.
`Actions` is always visible and opens the contextual action sheet; the sheet's title notes that Space opens it too.
There is no persistent action wall and no separate Detail, Printer Setup, or
Inventatory Scan Setup page.

Header status is shown as words. `docs/ui-style-guide.md` defines glyphs, alignment, controls,
colour, and status presentation for every workspace.

The retired phone/web scanner is not part of the Inventatory interface. The physical
Inventatory Scan R1 remains supported through the desktop application's authenticated
device service; UI copy calls this the R1 service rather than a generic bridge.

The minimum supported terminal is 100 by 30 cells. Smaller terminals show a
resize notice instead of a clipped interface.

Fresh-install setup is the sole shell exception. It uses a full-window,
centered terminal flow with the Inventatory wordmark and no navigation,
operational status, search, or action sheet. It confirms the data folder, offers
background mode and physical Inventatory Scan R1 setup, and leaves printer and
vendor configuration for Settings. The normal Scan R1 setup page remains
available after onboarding.

Setup wizards show only task-specific instructions, inputs, selections, and
actions. The release update wizard is the related full-window exception: it
uses a short release preview, bounded plain-text changelog, download progress
with speed and ETA, checksum verification, and a post-restart result screen.
Release notes are displayed as text only and are never interpreted as commands.

## Neutral graphite / Petrol-cyan palette

| Role | RGB / hex | Use |
|---|---|---|
| Canvas | `13,16,16` / `#0D1010` | Application background |
| Surface | `20,25,24` / `#141918` | Tables and content |
| Raised | `29,36,34` / `#1D2422` | Controls and overlays |
| Hover | `39,49,47` / `#27312F` | Mouse hover |
| Selection | `44,68,64` / `#2C4440` | Current row/cell/field |
| Divider | `56,69,67` / `#384543` | Necessary separators |
| Primary text | `241,238,229` / `#F1EEE5` | Important content |
| Secondary text | `202,208,202` / `#CAD0CA` | Labels |
| Muted text | `140,150,144` / `#8C9690` | Hints/inactive data |
| Interactive | `88,185,176` / `#58B9B0` | Actions, active status cues, and brand cues |
| Focus | `185,231,221` / `#B9E7DD` | Active focus |
| Link | `143,203,197` / `#8FCBC5` | External links and progress |
| Success | `165,201,165` / `#A5C9A5` | Ready/completed |
| Warning | `229,167,124` / `#E5A77C` | Attention/low stock (apricot) |
| Warning surface | `59,47,40` / `#3B2F28` | Low-stock highlights |
| Danger | `228,112,124` / `#E4707C` | Error/destructive/out (rose) |
| Danger surface | `67,38,43` / `#43262B` | Out-of-stock highlights |
| Active surface | `49,90,85` / `#315A55` | Scanner movement/build focus |

Petrol-cyan means interactive, active, focused, linked, or progressing. Ordinary
headings use warm ivory; graphite surfaces provide structure without making the
whole interface blue. Sage green, apricot, and rose are reserved for real state. Apricot and rose are
one warm family set against the cool teal, so attention reads as a different
kind of signal from interaction. Out of stock always says so in words; low stock
is the quantity itself in apricot. Normal rows share one surface; hover and
selection provide the only background changes.

Truecolor is the reference rendering. Terminals without truecolor may use their
nearest xterm-256 colors; text and glyphs keep all states understandable.

The normal `.\run.ps1` launch uses the classic Windows Console host
(`conhost.exe`). JetBrains Mono is the recommended font for the optional Windows
Terminal presentation. The repository includes `docs/windows-terminal-profile.json`
as a profile snippet; it is intentionally not installed or merged into the user's
terminal settings automatically. Developers can launch with
`.\run.ps1 -WindowsTerminal` after adding that profile as `Inventatory`.

Installed launches and in-app update restarts use the system's configured
terminal application, with the normal console host as a fallback.

## Input model

- `1`–`6`: switch workspace outside text entry
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

### Stock

One split workspace owns browsing, complete details, links, quantity changes,
printing, creation, and non-modal editing. Detail information is ordered by
decision value: name and description, electrical/rack/quantity essentials,
full electrical parameters, inventory identity, references, then notes. The
inventory list remains visible while editing. Save commits the entire working
copy; Escape cancels it.

The detail panel header and its action row sit outside the scrolling body, so
New/Edit/-/+/Delete/Print stay reachable at the bottom of the panel however
long the selected part is. Delete carries the danger role and arms a
confirmation popup rather than erasing immediately. Quantity and rack are a compact strip directly under the
summary, next to the buttons that change them, and are not repeated in the
identity block where the list columns already show them. Vendor and catalogue
identifiers sit in a collapsed IDENTITY disclosure whose heading states how many
fields are hidden. Fields with no value are omitted, including vendor
placeholders such as a lone dash or "N/A"; the package value appears once in the
passive summary line rather than again in the parameter list.

List rows split distributor-style names the same way (`splitPartName`): the leading
words are dim, the first electrical value is bold, and the specification is dim, so a run of
near-identical names can be scanned by value. A name with no recognizable value stays whole.
Quantities are bold and colored by state, and each category header carries its part count and
low-stock count. The detail panel leads with the full part name in bold, then the manufacturer,
then a raised quantity strip showing the quantity (with LOW or OUT) and the rack.

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
Every occupied slot splits its part name into a dim type, a bold value, and a dim
specification, with a bold quantity pinned to the bottom of the cell that is apricot
when low and rose (with the word OUT) when empty. In narrow cells the type is
dropped, because the rack's own type already implies it, and padding is removed so
the value stays whole. Lettered column headers and numbered row headers identify
each slot without adding labels inside the slot cells. Rows and cells are clickable.

Every slot has the identical width and height, and the grid fills the available
space exactly. The letter header (one row) and the row-number column never change
size. Rows left over by the floor division go to a summary band under the matrix
(one to five rows) and columns left over go to the detail panel, so no gap appears
at the bottom or right edge on any terminal size.

The slot detail panel leads with `Slot: A1` and the rack type, capitalized. An
occupied slot then shows the full part name in bold, the manufacturer, a raised
quantity strip (quantity, with LOW or OUT when it applies), the key specifications
the Stock page leads with, and the package. Controls are anchored at the bottom in
one row when the panel is at least 40 columns wide (Move, quantity buttons,
Datasheet, Remove), and in two rows when it is narrower; empty racks offer Delete
rack. The rack summary (slots used, low, out) lives in the footer band under the
grid. `d` opens the selected part's datasheet, as on Stock. The remaining
administrative operations remain in Actions.

### Projects

The project list shows bold project names, line and board counts, and a Built state. An open project compares the BOM
with stock in two groups, In Stock and Missing, each with per-category counts. Rows carry the part in bold, its package,
the rack slot or suggested match, Need in muted text, and Have in bold: primary when covered, apricot when partly
covered, rose when nothing is in stock. Status reads Ready or Short N. Find in racks reuses the Racks grid: lettered
columns, numbered rows, identical slots with the same slot body, and a footer band that takes the rows floor division
cannot hand out. Slots to open pulse; the side rail lists them with bold slot and part.

### Import

The empty state starts file selection. Imported rows are reviewed beside their
details, corrected inline, and accepted or skipped. DigiKey synchronization is
offered after review with an explicit completion choice.

### Settings

General/Data, Appearance, and Updates live under System. Devices contains Printer, with Quick Labels nested beneath
it, and Inventatory Scan. DigiKey remains under Integrations. Ordinary edits are staged and use Save/Cancel.
Refresh, Test, Copy, Regenerate, Clear, Check for software updates, and Update are operational actions. DigiKey secrets are stored by the operating-system credential service, never in `settings.conf`.

The Quick Labels panel owns the optional custom wire label as well as the saved presets. The Printer panel contains
printer discovery, queue testing, and the EU/US schematic symbol standard used on rack labels (`y` switches it).

Every panel follows one layout. A header row names the category and shows
Saved/Unsaved changes, followed by a divider. The body is a list of titled
sections separated by one blank line; a section title may carry right-aligned
meta such as a count or the last update check. Rows inside a section share a
three-column marker gutter (`>` when selected), a fixed label column, and a
value column, whether the row is an editable field, an On/Off toggle, a colored
status value, or a selectable list entry. A section's buttons come last in that
section, aligned under its labels. Save and Cancel stay in the shared footer.

Each panel carries only settings and state: no explanatory hint copy, and no
navigation button that duplicates a category already in the sidebar. An
unconfigured integration or device shows a single Setup section with its status
and a filled cyan `Begin Setup` button; setup guidance lives in its wizard. Once setup is accepted, the panel
restores its settings and state rows. Every other control is an ordinary raised
text button. Panels do not restate diagnostics that the device reports
elsewhere, and values derived from an absent device are omitted rather than
shown as zeros.

## Reusable UI rules

- Prefer whitespace and alignment over borders.
- Use a divider only between major functional regions.
- Controls are compact text labels on a raised surface, not decorative cards.
- Tables use fixed columns and right-aligned numbers.
- Hierarchy comes from weight and tone, not decoration: the thing to scan for is bold, its
  qualifiers are muted, and labels are quieter than their values. Do not add glyph ornaments
  such as dots or corner brackets.
- Selection combines a dark blue-gray background, focus-colored text, and a marker.
- Messages never stack; the newest message replaces the previous one.
- Confirmations use a raised surface, explicit danger text, and an unlock step
  where accidental activation would be costly.
