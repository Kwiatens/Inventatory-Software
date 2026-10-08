# Inventatory UI Style Guide

This guide covers how Inventatory's terminal interface looks and reads: glyphs, alignment,
controls, color, status, and guided workflows. Every page and overlay follows it.
`docs/ui-redesign.md` describes what each workspace contains. This guide describes how that
content is drawn. If the two disagree, follow this guide and update the other document in the
same change.

The goal is a calm, plain, aligned interface that a person can understand without being taught.
Use position, weight, and words to explain the interface. Do not use decoration for this.

## 1. Principles

1. **State over decoration.** Every visible element either shows state, offers an action, or
   separates regions. If an element does none of these, remove it.
2. **Words, not icons.** `Ready`, `Offline`, `Unsaved`, `Missing` are clearer than any symbol.
3. **One grammar.** The same kind of information looks the same on every page. A setting row in
   Printer and a setting row in General are drawn by the same rules.
4. **Alignment is structure.** Columns line up across rows and sections. Do not use borders,
   boxes, or ornaments where alignment would do.
5. **No explainers.** Do not add hint lines, helper sentences, or descriptive copy under
   settings or sections. If a label needs explaining, rename it. Setup guidance belongs in
   wizards, and long explanations belong in `docs/`.

## 2. Glyphs

Use ASCII text plus the box-drawing characters already used for dividers (`─ │ ┬ ┴ ┼ ├ ┤`).
Nothing else.

| Need | Use | Never |
|---|---|---|
| Selected row or current item | `>` in the marker gutter | `▌ ▶ ► ● →` |
| Sequence or route | `Rack 1 > Rack 2 > Finish` | `▸ → ➜ ›` |
| Empty value or list dash | `-` | `—` `•` `·` |
| Expand / collapse | `+ Details` / `- Details` | `▸ ▾ ▶ ▼` |
| Quantity change | `41 to 31` | `41 → 31` |
| Status | a word (`Ready`, `Error`, `Off`) | `● ○ ◆ ✓ ✗` dots or ticks |
| Chosen option | the option's text (`EU / IEC`) | `‹ EU ›`, `< EU >`, `[ EU ]` |
| Editable value | the value itself (`5 pcs`) | `[ 5 ]`, underscores, boxes |
| Progress | numbers (`15 of 40`, `Rack 1 of 4`) | block bars `█░`, spinners |
| Keys | words: `Enter`, `Esc`, `Bksp`, `Up/Down` | `↑↓ ← → ⏎` |
| Message severity | the message colour and wording | `[!]`, `[i]` prefixes |
| Secrets | `Hidden` | `••••••` |

Emoji, Nerd Font icons, and decorative Unicode are not allowed. If a glyph would need a legend,
it does not belong in the interface.

Existing code that still uses glyphs from the "Never" column should be migrated when that code
is next touched. Do not add new uses.

## 3. Layout and alignment

### Columns

Each page defines a small, fixed set of columns and draws every row against them.

- **Marker gutter**: one cell for `>` plus one space.
- **Label column**: left-aligned, fixed start.
- **Value column**: left-aligned, fixed start, shared by every row in the panel.
- **Action column**: buttons start at one fixed column and flow right with one cell between
  them. They are left-aligned to that column, so the first button in every row starts at
  the same position.
- **Numbers** are right-aligned in their own column. Units follow the number (`5 pcs`).

Do not right-align buttons whose widths differ, because their left edges then jump from row to
row. Do not centre controls, except in a modal dialog.

### One page skeleton

Every workspace is built from the same pieces in the same places. A page must not feel like
separate panels that were assembled afterwards.

- **One plane.** The whole workspace (rows between the navigation bar and the status line)
  is a single surface. Do not draw inner boxes, panels, or vertical dividers between content
  that belongs together. In Settings, the selected navigation item shares the content surface,
  so the item and its page read as one piece.
- **Two-line page header.**
  - Line 1 holds the title in bold. Its buttons sit at the right end as one group, with the
    primary button first. A pending state such as `1 unsaved change` sits directly to the left
    of that button group.
  - Line 2 holds the context in muted text: the status word and facts, or the project and route.
  - The page's main actions (`Save`, `Pick parts`, `Rack done`, `Deduct from stock`) always
    live in that header group. Never put them in a separate footer or float them elsewhere.
- **Content starts after one blank row** and uses the page's fixed columns from top to bottom.
- **Key hints** live only in the shared status line at the bottom of the window. They are muted
  and right-aligned. Do not add per-panel hint rows.

### Groups

- Settings groups are named in a muted gutter column on the first row of the group, not by a
  separate title row. One blank row separates groups.
- Tables group rows under a full-width group row on the raised surface: the name in its state
  colour, followed by a muted count.
- Related things sit next to each other. A list that describes a grid starts on the grid's
  header row and shares its top edge. A row's buttons sit on that row.
- Truncate paths in the middle (`~/Dokumenty/.../settings.conf`) and names at the end (`...`).
  A truncation must never cut a number or unit.

### Minimum size

Every layout must work at 100x30 cells. Check it at 100x30, 120x30, and a wide terminal.

## 4. Controls

There are exactly five kinds of value on a row. Their kind decides how they look.

| Kind | Looks like | Activation |
|---|---|---|
| Toggle | `On` / `Off` in primary text | Enter flips it |
| Choice | the chosen option in primary text | Enter opens a list of options |
| Editable value | the value in primary text | Enter opens an inline editor |
| Read-only | secondary or muted text | none |
| Action | a button in the action column | Enter or click runs it |

The difference between editable and read-only is shown by tone (primary versus
secondary/muted), never by brackets.

### Buttons

- A button is ` Label ` on the raised surface, optionally followed by its key in muted text:
  ` Test print t `.
- At most one **primary** button per region uses the active surface (for example `Save`,
  `Pick parts`, or `Rack done`). The primary button comes first.
- **Destructive** buttons (`Discard`, `Forget`, `Delete`, `Restore`) use danger-coloured text
  on the normal raised surface. Do not fill them with red. They always open a confirmation.
- Labels are verbs or short noun phrases in sentence case: `Back up now`, `Pair new`,
  `Shopping list`. Do not add trailing ellipses or arrows.

### Selection and focus

- A focused row gets the selection background across the full content width and a `>` in the
  marker gutter. Focus must always be visible. A key that moves focus must change something on
  screen.
- Up/Down move between rows. Left/Right move between sections or panes. Enter activates. Esc
  steps back one level, and asks before throwing away unsaved changes.
- Moving the cursor never changes a setting. Selecting from a list stages a change only when
  the user presses Enter.

## 5. Status

- Each settings section and each device or integration starts with a **Status** row. Its value
  is one coloured word (`Ready`, `Offline`, `Error`, `Not set up`), followed by short muted
  facts (`idle, last label 12:31`).
- The settings navigation shows a short word right-aligned beside a section only when that
  section needs attention: `New`, `Error`, `Unsaved`, `Off`. Healthy sections show nothing.
- Counts are numbers with nouns (`28 missing`, `7 of 12`). Never show a bare icon.

## 6. Unsaved changes

- A changed value is drawn in the warning colour, followed by muted `was <old value>`.
- The page header shows `1 unsaved change` / `N unsaved changes` in the warning colour.
- `Save s` and `Discard Esc` appear in the header's button group only while something is
  unsaved.
- Actions that take effect immediately (print, export, back up, restore) are buttons, never
  staged values, so the user can always tell what Save will do.

## 7. Colour

Colour carries meaning. It is never there for decoration. Always use the semantic helpers in
`src/ui/shared/AppUiShared.{h,cpp}`. Do not write literal RGB values.

| Role | Meaning |
|---|---|
| Canvas / Surface / Raised | Structure: background, content, controls |
| Selection / Hover | Where the user is |
| Primary / Secondary / Muted text | Importance: value, label, qualifier |
| Interactive / Focus / Link | Something can be acted on or is focused |
| Active surface | The primary button and the items a guided workflow points at |
| Success | Healthy or complete state (`Ready`, `Saved`) |
| Warning | Needs attention soon (`Low`, `Unsaved`, update available) |
| Danger | Broken, missing, or destructive (`Error`, `Missing`, `Forget`) |

Rules:

- One status colour per row at most. Colour the status word or the number, never the whole
  row.
- Status backgrounds (warning, danger, flash) are reserved for banners and confirmations. Do
  not use them on ordinary rows.
- Status colours must sit in the same calm family as the petrol-cyan accent. In OKLCH, keep
  status text at lightness 0.70 to 0.86 and chroma at or below the interactive colour's chroma
  (about 0.09). Warning is a pale straw/sand yellow (hue about 85 to 95), not orange. Danger is a
  muted rose (hue about 350 to 20). Success is a sea-glass green (hue about 160 to 175). Saturated
  oranges, pure reds, and bright greens clash with the blue base and are not allowed.
- The chosen status palette is **Slate**. Its text colours are also cooled so that nothing
  reads warm against the petrol-cyan base:

  | Role | Hex |
  |---|---|
  | Primary text | `#E5EDEC` |
  | Secondary text | `#BFCAC9` |
  | Success | `#A0D0C0` |
  | Warning | `#DBCDA9` |
  | Danger | `#CC8BA4` |
  | Warning background | `#2A261C` |
  | Danger background | `#322027` |
  | Danger flash background | `#583744` |

- Default values live in `src/app/settings/AppSettings.h`. When a default changes, add the
  previous default to `upgradeLegacyAppearanceDefaults` so that installs which never customised
  the colour move to the new value.

## 8. Motion

- Animation may draw attention, but it must never hide information. A pulse alternates between
  two visible states (for example active surface and interactive surface) and never fades to the
  unhighlighted look.
- Do not use spinners or animated progress bars. Show progress as text that updates.

## 9. Guided workflows

Find in racks, setup wizards, and imports guide the user through physical or multi-step work.

- Lead with one plain instruction that holds the numbers that matter:
  `Take 26 parts from 7 slots in Rack 1`.
- Confirm work in natural units. In Find in racks, that unit is a rack, not a slot: everything
  to take from the current rack is shown at once, and one key confirms the rack.
- Highlighted targets show what to do there (`Take 10`), not stock levels.
- Use one notation for values in the list and the grid.
- Never mix things that cannot be done into a list of things to do. Missing parts belong to the
  shopping list, not to the pick route.
- The end of a workflow asks one clear question with the consequence in numbers
  (`Deduct 40 parts`) and offers a way to correct amounts before committing.

## 10. Wording

- Sentence case for labels, titles, and buttons. Do not end labels with a colon.
- Name settings by their effect: `Label after each scan`, not `Auto-label`;
  `Keep running when closed`, not `Background & startup`.
- Dates are `YYYY-MM-DD HH:MM`. Quantities carry units where they are ambiguous.
- Use the same word for the same thing everywhere (`Inventatory Scan`, `R1 service`).

## 11. Voice

Inventatory speaks like a calm colleague standing next to the rack. It is not a character or a
chat. The second line of each page header is one or two plain sentences about the situation
right now:

- `Rack 1: take 26 parts from 7 slots`
- `You have 15 of 43 parts. The other 28 need ordering, 21 have a DigiKey match.`
- `The R1 is online, but 2 scans could not be saved. Retry them or discard them.`
- `Everything is saved. 94 parts in 9 racks.`

Rules:

1. Report state and the next step. Do not explain how a feature works; that stays an
   explainer and stays out (section 1).
2. Do not use `I`, a name, exclamation marks, praise, jokes, or emoji.
3. Be concrete: real counts, part values, slot names. Bold the numbers and names the user acts
   on. Use a status colour only on the phrase that carries the state.
4. Use at most two sentences, and only in the header's second line and in the transient message
   line. Never put them inside setting rows, table cells, or buttons.
5. When nothing needs attention, give a plain fact (`Everything is saved.`), not filler.
6. Build each sentence from a fixed template in code and fill it from live data. The same state
   always produces the same words, nothing is guessed, and each template has a unit test.
7. Sentences must fit the header at 100 columns. Write a shorter variant for narrow
   terminals rather than wrapping.

## 12. Review checklist

- [ ] No glyphs outside ASCII and box-drawing dividers; no dots, arrows, chevrons, ticks, or
      bracketed values.
- [ ] The page uses the shared skeleton: one surface, a two-line header with the main actions
      at its right end, no inner boxes or per-panel footers.
- [ ] Labels, values, numbers, and buttons align to the page's fixed columns.
- [ ] Every row is one of the five control kinds and looks like it.
- [ ] Focus is visible after every navigation key. Moving the cursor never changes data.
- [ ] Status is a word, and it uses at most one status colour per row.
- [ ] Unsaved changes are visible on the row and in the header.
- [ ] Header sentences follow the voice rules and come from tested templates.
- [ ] Destructive actions use danger text and a confirmation.
- [ ] No explainer copy has been added.
- [ ] Checked at 100x30, 120x30, and a wide terminal.
