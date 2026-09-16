# Inventatory GUI/UX Polish Design

## Scope

Implement the requested GUI/UX polish from `main` at `b8f3afe` while preserving
Inventatory's terminal-first workflow, neutral graphite / petrol-cyan palette,
keyboard-first interaction model, mouse parity, contextual Actions system, and
existing state/persistence ownership. Item 4 from the earlier audit is
explicitly excluded: no new header live state, clock redesign, or persistent
Actions placement.

## Design

The existing `App::target` boundary remains the single place that translates
`UiTargetKind`, hover, keyboard focus, enabled state, and activation into visual
feedback. The implementation will give navigation, action-sheet entries,
fields, primary buttons, secondary buttons, rows, cells, and disabled controls
distinct treatments using the existing semantic palette. Row and cell focus
will retain selection background/content contrast and add a small marker where
the geometry allows it; persistent selection and temporary focus will not be
collapsed into one visual state. Filled primary controls will keep their cyan
surface under hover and focus.

The existing fixed bottom shell row remains the only transient message surface.
Messages gain a small semantic severity value (info, success, warning, or
error), a stable ASCII-safe prefix, and a short acknowledgement pulse followed
by stable semantic coloring. Existing progress/enrichment rows keep precedence
and behavior. The old two-argument message call remains valid with an
appropriate default so migration can be incremental; obvious save/create/
update, unavailable-operation, and persistence call sites will opt into the
right severity without changing workflow semantics.

Ordinary data rows will use the shared surface background. Group headers,
disclosure headers, warning/danger states, active rack movement, selected rows,
and hover/focus remain distinct. The rack list will become a real bounded
scrolling region with a continuation indicator and selection kept in view;
other pages will receive only the focused consistency fixes needed to match
existing Stock/History/Import/Home patterns.

Home will omit the optional context row only when idle; prompt/search/action
modes keep their context UI and the fixed message row remains reserved. The
scanner panel will animate only while meaningfully online; offline, unpaired,
and waiting states will be concise and static. Home metrics will emphasize
numeric values with weight while labels stay muted, and equivalent quantitative
values across the other reviewed pages will use restrained alignment/spacing
improvements.

## Ownership and integration

Four independent worktrees branch from the same base commit:

1. Shared interaction/message/shell: `App.h`, `AppInput.cpp`,
   `AppInventorySelection.cpp`, `AppShellRender.cpp`, and `ActionRegistry.cpp`.
2. Stock/Racks: Stock render/list files and Rack render/private helpers.
3. Home: dashboard render/components/private helpers.
4. Cross-page presentation: Projects/BOM, History, Import, and Settings render
   files.

No two agents edit the same source file. Shared helper additions are allowed
only when they are genuinely reusable and are integrated deliberately. The
controller will review each branch, integrate the commits into one isolated
feature worktree, then run the complete documented Release build and CTest
suite plus the real desktop visual checks at 100x30, 120x30, and a wide or
maximized terminal.

## Verification

Focused tests will cover the new message severity/presentation contract and
any pure layout/scroll helper behavior added by the implementation. The final
verification will also check keyboard and mouse parity, action-sheet behavior,
selection/focus distinction, disabled controls, fixed-height messages, static
idle scanner states, scroll indicators, and clipping/wrapping at all required
terminal sizes. No screenshots or local runtime data will be committed.
