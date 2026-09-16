# Inventatory GUI/UX Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the requested GUI/UX polish items 1, 2, 3, 5, 6, 7, and 8 in one reviewed isolated feature branch while leaving item 4 and all backend/security behavior unchanged.

**Architecture:** Keep `App::target` and the four-region shell as the interaction and layout boundaries. Add the smallest shared message-severity contract needed for semantic feedback, then keep row, scroll, scanner, and typography changes in their owning page renderers. Four parallel implementation worktrees have disjoint source-file ownership; the controller integrates and rechecks their behavior in a final feature worktree.

**Tech Stack:** Windows C++17, FTXUI, CMake/NMake or Visual Studio Release build, assert-based `inventatory_tests`, `inventatory_input_tests`, and real visible console capture for UI acceptance.

**Spec:** `docs/superpowers/specs/2026-09-16-inventatory-gui-ux-polish-design.md`

## Global Constraints

- Preserve the terminal-first neutral graphite / petrol-cyan palette and existing keyboard-first plus mouse-parity workflow.
- Do not implement item 4: no header live-state redesign, clock redesign, or persistent Actions placement.
- Keep the fixed bottom message/progress row; messages never stack and the newest transient message replaces the prior one.
- Use existing semantic color helpers; do not add arbitrary color families, decorative cards, gradients, ornamental frames, or excessive separators.
- Normal rows share one surface; hover and selection provide interaction backgrounds while warning/danger/group/active states remain semantic.
- Keep the UI usable at the 100x30 minimum and verify 100x30, 120x30, and a wide/maximized terminal after render changes.
- Preserve persistence, database, scanner transport/security, DigiKey, printer, updater, and business logic; change only UI message severity at call sites.
- Every clickable target retains a keyboard route and remains discoverable through the contextual action system where it is a command.
- Do not modify scanner firmware, generated build output, local inventory data, credentials, or screenshots.

---

### Task 1: Shared interaction states, semantic messages, and shell chrome

**Files:**
- Modify: `src/App.h` (target/message declarations and message state)
- Modify: `src/app/shell/AppInput.cpp` (`App::target`, mouse/focus-compatible visual treatment)
- Modify: `src/app/inventory/AppInventorySelection.cpp` (message lifecycle)
- Modify: `src/app/shell/AppShellRender.cpp` (message rendering, action/context rows)
- Modify: `src/ui/ActionRegistry.cpp` (action-sheet row treatment and obvious message call sites)
- Test: `tests/inventatory_tests.cpp` (message severity/presentation helpers and any focused pure behavior)

**Interfaces:**
- Consumes: existing `UiTargetKind`, `target(...)`, `setMessage(text, seconds)`, `message_`, `messageUntil_`, `messageFlashStartedAt_`, and the existing semantic UI color helpers.
- Produces: a backward-compatible message API with an explicit severity overload/default, stable severity-to-prefix/color behavior used by `renderMessageUi()`, and target-kind-specific rendering that preserves existing activation and focus indices.

- [ ] **Step 1: Write the failing focused assertions**

  Add assertions for the desired severity-to-prefix/color mapping and the acknowledgement-pulse boundary using the smallest pure helper exposed by the shared/application UI code. Cover info, success, warning, error, the legacy default path, and the fixed-height quiet/message rows. Do not assert only implementation details; assert the rendered contract values that a later renderer uses.

- [ ] **Step 2: Run the focused test target and verify the expected RED failure**

  From the feature worktree, initialize the supported Visual Studio environment if needed, build `inventatory_tests`, and run it. The new assertions must fail because the severity contract does not yet exist; fix test typos if the failure is unrelated.

- [ ] **Step 3: Implement the minimal shared message and target-state contract**

  Add a small severity enum/state field and an overload that keeps existing two-argument callers valid. Make the message renderer choose an ASCII-safe semantic prefix and existing info/success/warning/danger color, with a single short 150–300 ms pulse and a fixed one-line footprint. Keep DigiKey/import/BOM progress rows and persistence/shortage precedence intact. Refine `App::target` by target kind so disabled targets stay muted/inert, primary buttons retain cyan identity, and row/cell focus can show a compact marker without changing hit boxes or activation order. Keep action-sheet and shell context rows on their established surfaces.

- [ ] **Step 4: Migrate only obvious important message call sites in this owned set**

  Mark successful action-sheet operations as success, invalid/unavailable operations as warning or error as appropriate, and persistence failures as error. Leave ordinary notices on the compatible default path and do not alter action IDs, callbacks, persistence, or business results.

- [ ] **Step 5: Run focused tests and the input test executable**

  Rebuild the touched test targets and run the focused test executable plus `inventatory_input_tests`. Confirm the new assertions pass and no input-routing assertions regress.

- [ ] **Step 6: Commit the task**

  Commit only the owned source/test files with an imperative Conventional Commit such as `feat(ui): add semantic interaction feedback`.

### Task 2: Stock and Racks surfaces and scrolling

**Files:**
- Modify: `src/ui/pages/stock/StockPage.cpp`
- Modify: `src/ui/pages/stock/StockPageList.cpp`
- Modify: `src/ui/pages/stock/StockPageDetail.cpp` only if a directly adjacent ordinary-row treatment requires it
- Modify: `src/ui/pages/racks/RackManagementPage.cpp`
- Modify: `src/ui/pages/racks/RackManagementPagePrivate.h`
- Test: `tests/inventatory_tests.cpp` only for pure rack viewport/row helper behavior if a new helper is introduced

**Interfaces:**
- Consumes: existing `yframe`, `vscroll_indicator`, `target`, `rackSelection_`, `rackRow_`, `rackColumn_`, `sortedRackIndices()`, and shared surface/selection helpers.
- Produces: ordinary Stock rows on one surface, a bounded Rack list with visible continuation indication and selection kept in view, and unchanged rack/grid activation behavior.

- [ ] **Step 1: Add a focused failing helper/layout assertion if needed**

  If the implementation extracts a pure rack viewport calculation, first add the smallest assertion showing that a selected rack outside the viewport yields a valid scroll offset/visible range and that the indicator is reserved without clipping the 100-column geometry. If no pure helper is needed, record the existing target/scroll behavior as the manual test boundary and do not add a speculative test abstraction.

- [ ] **Step 2: Run the focused test or baseline input test and observe RED when a new helper exists**

  Run the exact focused test case before adding the helper. For renderer-only edits, run the existing input test baseline and retain its output as the pre-change evidence.

- [ ] **Step 3: Remove ordinary zebra choices and keep semantic/group backgrounds**

  Make unselected ordinary Stock rows use the intended surface, preserving grouped headers, match bands, selection, hover/focus, warning/danger quantity semantics, and rack movement highlighting. Avoid changing column widths or business sorting.

- [ ] **Step 4: Bound the Rack list and keep selection visible**

  Wrap the rack-list body—not its fixed header—in the established FTXUI scrolling/indicator pattern. Reflect the list bounds if the existing mouse-wheel routing needs it, keep the selected rack in view during Up/Down/j/k and mouse wheel movement, and keep the grid/detail panels and 100-column width calculation unchanged except for the indicator lane.

- [ ] **Step 5: Run focused tests/build and inspect the diff**

  Build the affected targets, run `inventatory_input_tests` and any focused rack test, then check for clipping, accidental row flattening, or changed activation order in the diff.

- [ ] **Step 6: Commit the task**

  Commit only the Stock/Racks files with a scoped message such as `fix(ui): steady stock and rack list surfaces`.

### Task 3: Home scanner, dashboard lists, and metric hierarchy

**Files:**
- Modify: `src/ui/pages/dashboard/DashboardPageRender.cpp`
- Modify: `src/ui/pages/dashboard/DashboardPageComponents.cpp`
- Modify: `src/ui/pages/dashboard/DashboardPagePrivate.h` only if a pure dashboard helper needs a declaration
- Test: `tests/inventatory_tests.cpp` for dashboard snapshot/presentation helper behavior only when directly needed

**Interfaces:**
- Consumes: existing `ScannerDashboardState`, `scannerPanel`, `scannerActivityBar`, `uiAnimationTicks()`, `DashboardSnapshot`, `attentionPanel`, `activityPanel`, and dashboard selection wrappers.
- Produces: static idle offline/unpaired/waiting copy, connected-only activity animation, one-surface ordinary dashboard rows, and stronger numeric metric emphasis with unchanged data counts and selection routes.

- [ ] **Step 1: Add or extend the smallest failing dashboard presentation assertion**

  Cover stable copy for offline/unpaired/waiting states and the semantic distinction between dashboard row backgrounds and selected active rows. Keep snapshot/data tests separate from visual styling assertions.

- [ ] **Step 2: Run the focused test and verify RED**

  Run the dashboard-focused test before implementation; the old alternating/offline behavior must fail the new contract while existing snapshot assertions remain meaningful.

- [ ] **Step 3: Remove idle motion and normalize ordinary row surfaces**

  Replace timer-driven disconnected prose with concise static state copy. Preserve the connected ONLINE activity visualization and transition sizing. Remove alternating canvas/surface fills from attention/activity rows while retaining warning/danger text, selected-row treatment, panel grouping, and scroll indicators.

- [ ] **Step 4: Strengthen Home metrics with weight and alignment**

  Make numeric values visually dominant using existing header/body helpers or bold, keep labels muted/secondary, and preserve semantic warning/success/danger colors. Do not add cards, new colors, or extra content.

- [ ] **Step 5: Run focused tests and input checks**

  Rebuild and run dashboard/input coverage, checking that Left/Right panel switching, arrows, j/k, PageUp/PageDown, Home/End, and mouse-wheel panel targeting remain unchanged.

- [ ] **Step 6: Commit the task**

  Commit only dashboard files and focused tests with a message such as `fix(ui): calm dashboard idle states`.

### Task 4: Cross-page list consistency and micro-hierarchy

**Files:**
- Modify: `src/ui/pages/bom/BomProjectPageRender.cpp`
- Modify: `src/ui/pages/history/HistoryPageRender.cpp`
- Modify: `src/ui/pages/import/ImportCsvPage.cpp`
- Modify: `src/ui/pages/settings/SettingsPage.cpp`
- Modify: `src/ui/pages/settings/SettingsPageAppearance.cpp` only for a directly adjacent low-risk value/hint hierarchy fix
- Test: `tests/inventatory_tests.cpp` only for pure layout formatting behavior introduced here

**Interfaces:**
- Consumes: existing Projects/History/Import/Settings renderers, page-local `yframe`/`vscroll_indicator` patterns, shared semantic typography/color helpers, and existing target/action registrations.
- Produces: focused removal of remaining ordinary zebra fills, consistent scroll indicators where content can exceed its region, and right-aligned/demoted quantitative/hint values without information-architecture changes.

- [ ] **Step 1: Identify exact current patterns and write one focused failing assertion per new pure formatter**

  Inspect the four owned renderers and add tests only for extracted pure formatting/geometry helpers. Do not make renderer tests depend on a live terminal or mutate application data.

- [ ] **Step 2: Run the focused assertion before implementation**

  Confirm the new assertion fails for the current formatting contract, or, if no helper is appropriate, run the existing input test and record the unchanged baseline.

- [ ] **Step 3: Normalize ordinary surfaces without flattening structure**

  Remove alternating fills from ordinary Projects/History/Import/Settings rows while preserving group headers, status/warning/danger surfaces, disclosures, selected rows, and separators that communicate real structure.

- [ ] **Step 4: Fix only obvious scroll and hierarchy inconsistencies**

  Match established Stock/History/Import/Home scrolling patterns for long interactive regions, keep selected entries visible, and do not add scrollbars to fixed short panels. Right-align counts/quantities where the existing columns support it, mute hints that compete with values, and make equivalent controls share the existing spacing/prominence.

- [ ] **Step 5: Run focused tests and input checks**

  Build and run the affected test targets, then verify that Projects Find in racks, History drill-down, Import review, and Settings navigation retain their existing keyboard, mouse, action-sheet, and overlay routes.

- [ ] **Step 6: Commit the task**

  Commit only the cross-page renderer files and focused tests with a message such as `style(ui): align cross-page hierarchy`.

### Task 5: Review and integrate the four implementation branches

**Files:**
- Modify: the integrated feature worktree only; resolve overlap deliberately if any agent violates the declared write set
- Test: `tests/inventatory_tests.cpp`, `tests/inventatory_input_tests.cpp`, and all registered CTest targets

**Interfaces:**
- Consumes: the four reviewed task commits, the design spec, and each task's focused test evidence.
- Produces: one integrated `codex/gui-ux-polish` branch/worktree with no item 4 changes, no generated artifacts, and a clean reviewable diff.

- [ ] **Step 1: Record base and inspect every agent report/diff**

  Verify each branch/worktree is based on `b8f3afe044b4f9f1dda60236fac8127dbaf661d9`, review the changed file list and full diff, and resolve any write-set overlap before integration.

- [ ] **Step 2: Integrate the commits into the coordinator worktree**

  Apply/cherry-pick only the reviewed task commits in dependency order: shared contract first, then page-local changes. Keep the working tree isolated and leave `main` untouched.

- [ ] **Step 3: Run the documented Release build and all tests**

  Initialize `vcvars64.bat`, build `inventatory`, `inventatory_background`, `inventatory_tests`, and `inventatory_input_tests` in the ignored `build-gui-ux` directory, then run `ctest --test-dir build-gui-ux -C Release --output-on-failure` and `git diff --check`. Inspect any failure rather than weakening assertions.

- [ ] **Step 4: Request and act on the broad whole-branch code review**

  Give a fresh reviewer the complete merge-base-to-HEAD diff plus this spec/plan. Fix Critical/Important findings through one scoped agent fix/re-review cycle, and record any deferred minor or ruled finding in the SDD ledger.

- [ ] **Step 5: Launch and inspect the actual Release UI**

  Use a fresh agent-owned visible Release console, not a pseudoconsole reconstruction. Exercise Home, Stock, Racks, Projects, History, Import, Settings, Actions, hover, Tab/Shift+Tab, Enter, list navigation, scrolling, and at least one success/warning/error message. Capture and inspect window-only desktop pixels at 100x30, 120x30, and a wide/maximized terminal, checking clipping, wrapping, focus/selection ambiguity, indicator overlap, fixed-height messages, disabled-control appearance, and contrast.

- [ ] **Step 6: Re-run tests after visual fixes and prepare handoff**

  Re-run the full build/test sequence after any visual correction, review status and diff for unrelated files, and only then use the finishing-a-development-branch workflow to present integration choices.
