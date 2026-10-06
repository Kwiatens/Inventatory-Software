# Slice J: UI pages + ActionRegistry + shell input/render

Reviewed on main @ 0ce3761 (includes the UTF-8 text-input fix). Static review only, nothing built or run. Layout/overflow claims
(J2, J5, J12) rest on my knowledge of FTXUI layout semantics, not on a capture; confidence is stated per finding.

## Coverage

Read in full:
- src/ui/ActionRegistry.cpp
- src/ui/shared/AppUiShared.{h,cpp}
- src/ui/pages/stock/*: StockPage, StockPageList, StockPageDetail, StockPageInput, StockPageStocktake, StockFilterState.h
- src/ui/pages/racks/RackManagementPage.cpp and RackManagementPagePrivate.h
- src/ui/pages/history/HistoryPage.cpp, HistoryPageRender.cpp, HistoryPagePrivate.{h,cpp}
- src/ui/pages/import/ImportCsvPage.cpp
- src/ui/pages/bom/BomProjectPage.cpp and BomProjectPageRender.cpp
- src/ui/pages/onboarding/OnboardingPage.cpp
- src/ui/pages/scanner/InventatoryScanSetupPage.cpp and ...PageRender.cpp
- src/ui/pages/digikey/DigiKeySetupPage.cpp
- src/ui/pages/printer/PrinterSetupPage.cpp
- src/ui/pages/settings/SettingsPage.cpp, SettingsPageInput.cpp, SettingsPageState.cpp, SettingsPageAppearance.cpp, SettingsPagePrivate.h
- src/app/shell/AppShell.cpp, AppInput.cpp, AppInputModes.cpp, AppShellRender.cpp, AppNavigation.h
- docs/ui-redesign.md, docs/user-guide.md, docs/ui-contributor-guide.md

Read in part (cross-reads needed to trace callers):
- AppUiItemDetails.cpp (all of the logic; only the long per-category parameter tables skimmed)
- SettingsPageSave.cpp (lines 95-150 only; save/rollback belongs to slice I)
- AppRuntime.cpp (requestUserExit, restartDeviceService, completeSettingsExit)
- AppRackActions.cpp (changePage)
- AppInventoryEdit.cpp, AppInventorySelection.cpp, AppQuickLabelActions.cpp (stock filter handlers)
- AppBomBuildActions.cpp, AppBomProjectActions.cpp, AppBomActions.cpp (activePrompt)
- AppUpdate.cpp (handleUpdateKey)
- Console.h (appendKeyText, eraseLastCharacter)

Not read: AppBootstrap.*, the Update-wizard render body in AppUpdate.cpp (slice I), SettingsPageSave.cpp beyond the lines above.

Already-known items (T1, T14/T15/T20, T26, T28) are not repeated. Where a finding touches T14/T15/T20's root cause it says so.

## Findings

### [S1] `uiTargets_` is reallocated mid-frame while FTXUI `reflect()` holds `Box&` into it (use-after-free write once a page has more than 512 targets)
- Location: src/app/shell/AppShellRender.cpp:73-74, src/app/shell/AppInput.cpp:272-273
- Category: correctness (memory safety)
- Failure scenario: `renderUi()` clears `uiTargets_` and calls `reserve(512)`. `App::target()` then does `uiTargets_.push_back(...)` followed by
  `element | ftxui::reflect(uiTargets_.back().bounds)`. FTXUI's `reflect(Box&)` stores a reference to that Box, and the Box is written when the
  element tree is laid out, after `renderUi()` has returned. If any frame creates more than 512 targets, the 513th `push_back` reallocates the
  vector and frees the old buffer. Every `Reflect` node created for targets 0..511 now holds a dangling reference.
  Pages that build one target per data row cross 512 easily:
  - Stock: every inventory row plus about 25 chrome targets. Roughly 490 parts is enough.
  - History: one target per commit (`history.commit.<id>`). `inventoryCommits_` is loaded whole and never capped. Every saved stock change is a commit, so this is reached after a few months of use.
  - Import: one row per CSV line.
  - History detail: one per changed record.
  Effects:
  1. The layout pass writes 16-byte Boxes into freed memory. The old buffer is about 45 KB and is likely to be recycled by the
     many FTXUI nodes allocated later in the same frame, which is heap corruption.
  2. The bounds of targets 0..511 (header navigation, filter button, first rows) are never updated in the live vector, so
     mouse hit-testing on them is wrong for that frame.
  The vector keeps its larger capacity afterwards, so this repeats only at each doubling (512, 1024, 2048...). That makes it sporadic and hard to reproduce, and it would show up under ASan or a debug allocator.
- Evidence:
  ```
  uiTargets_.clear();
  uiTargets_.reserve(512);                       // AppShellRender.cpp:73
  ...
  uiTargets_.push_back(UiTarget{move(id), kind, {}, enabled, focusable, move(activate)});
  return element | ftxui::reflect(uiTargets_.back().bounds);   // AppInput.cpp:272
  ```
  `mutable std::vector<UiTarget> uiTargets_;` (App.h:1077)
- Confidence: high on the mechanism (reference into a growing vector); medium on how often it visibly crashes.
- Proposed test: a unit test that builds N > 512 targets through a small harness and checks that each reflected Box lands in `uiTargets_[i].bounds`
  (or, simpler, run a Stock render with 1000 items under ASan).
- Proposed fix (sketch): store `std::shared_ptr<ftxui::Box>` (or `std::deque<UiTarget>`, whose element references stay stable) and reflect that.
  Alternatively make `bounds` a `shared_ptr<Box>` and look targets up by index afterwards. Also virtualise or cap long lists so a frame does not build thousands of targets.

### [S2] The action sheet has no on-screen affordance and no mouse route; Space is the only way in and nothing tells the user
- Location: src/app/shell/AppShellRender.cpp:155-199 (`renderHeaderUi`), src/app/shell/AppInput.cpp:188-191; docs/ui-redesign.md:18, docs/user-guide.md:5-8
- Category: ui-parity
- Failure scenario: docs/ui-redesign.md says "`Actions · Space` is always visible and opens the contextual action sheet", and the user guide says "Press `Space` or click `Actions`".
  The header renders only the brand mark, the six navigation items and a clock. A search for "Actions" or "Space" in src/ finds no such label or target anywhere.
  A mouse-only user cannot open the sheet at all. A keyboard user has no on-screen hint that Space exists.
  A large share of commands is reachable only through the sheet or through undiscoverable keys:
  export, backup, undo, jump to rack, create/rename/delete rack, restore, token regeneration and so on.
  This breaks the AGENTS.md invariants "fully mouse-usable" and "every accelerator must remain discoverable on screen or in Actions".
- Evidence:
  ```
  header.push_back(ftxui::hbox(move(navigation)));
  header.push_back(ftxui::filler());
  header.push_back(uiBodyText(" " + currentDateTimeText() + " ", uiSecondaryText()));
  ```
  (comment at AppShellRender.cpp:70: "press space to open the full action sheet" is the only mention)
- Confidence: high.
- Proposed test: a render-level assertion (or a manual 100x30 capture) that a `Navigation` or `Action` target with id "actions" exists, and that
  its activation calls `openActionSheet()`.
- Proposed fix (sketch): add a right-aligned `Actions · Space` target to the header, before the clock, wired to `openActionSheet()`. Keep Space as the keyboard route.

### [S2] Stock "Receive shortage" quantity entry has no on-screen echo, and the buffer is pre-filled invisibly, so digits append to a hidden default
- Location: src/app/shell/AppShellRender.cpp:233-240 (`showsPrompt` list), src/app/bom/AppBomActions.cpp:182-195 (`activePrompt`), src/app/bom/AppBomProjectActions.cpp:378-381, src/app/shell/AppInputModes.cpp:207-226
- Category: correctness (wrong inventory quantity)
- Failure scenario:
  1. On Projects, select a missing line and press `r` (or Enter).
  2. `beginBomRestock()` sets `inputBuffer_ = to_string(needed - available)` (for example "5") and `inputMode_ = BomRestock`.
  3. `InputMode::BomRestock` is in neither `showsPrompt` nor `activePrompt()`, and the Projects page renders no input. The context line keeps showing "N lines · M boards", so the prefilled value and everything typed afterwards are invisible. The only hint is a 4-second message that disappears.
  4. The user types "10" intending to receive 10 and gets "510". Because nothing is displayed, they cannot notice. Enter then adds 510 to the stock quantity and writes a history commit.
  An accidental Enter without typing silently receives the default shortage quantity, again without the user ever seeing a number.
- Evidence:
  ```
  inputBuffer_ = to_string(max(1, match.needed - match.available));   // AppBomProjectActions.cpp:379
  inputMode_ = InputMode::BomRestock;
  ...
  inputMode_ == InputMode::QuantityAdjust || inputMode_ == InputMode::StocktakeCount ||   // showsPrompt: no BomRestock
  ```
- Confidence: high.
- Proposed test: for every `InputMode` that consumes characters (`handle*Key` appends to `inputBuffer_`), assert `activePrompt()` is non-empty and `showsPrompt` is true.
  A table-driven check would have caught this.
- Proposed fix (sketch): add `BomRestock` to `showsPrompt` and `activePrompt()` ("Received quantity: "). Better, select-all-and-replace semantics: the first typed digit replaces the default, as with the stock "set quantity" flow.

### [S2] Keyboard focus is an index into the per-frame target list and is set by mouse clicks, so it swallows Enter and jumps selection back
- Location: src/app/shell/AppInput.cpp:157-159, :334-336, :367-374; none of the list pages reset `focusedTarget_` when the selection moves
- Category: correctness / ui-parity
- Failure scenario: a left click stores `focusedTarget_ = <index of the clicked target>`. `handleKey` calls `activateFocusedTarget()` for Enter before
  registry actions and page handlers, and nothing clears the focus on arrow or j/k navigation. So any click on a row or cell followed by keyboard use misbehaves:
  - Import review: click row 3, press `j` (selection moves to row 4), press Enter to accept. Enter re-activates the row-3 target. The selection jumps back to row 3 and nothing is accepted. The documented "Enter accept" never fires until the user leaves the page.
  - Racks: click a slot, move with the arrows, press Enter ("part details" registry action). Enter re-selects the clicked slot.
  - Stocktake: after clicking a part, Enter ("count selected part") only re-selects the row instead of opening the count prompt.
  - Projects Split: Enter ("restock the selected shortage") is swallowed the same way.
  Two rows also render highlighted at once (the stale focus row and the selected row).
  The index is also not stable when the target set changes between frames. After a search filter or a new scanner row, the same index points at a different control.
- Evidence:
  ```
  focusedTarget_ = static_cast<int>(reverse - 1);
  hit.activate();                                                     // handleMouse
  ...
  if (key.type == KeyType::Enter && activateFocusedTarget()) { return; }   // handleKey, before dispatchAction
  ```
- Confidence: high on the mechanism (traced for Import, Racks, Stock, Projects).
- Proposed test: with a mock target list, click target k, send Down, then send Enter. Assert the page's Enter action ran and the selection did not move back.
- Proposed fix (sketch): track focus by target id, not index. Clear it on any non-Tab navigation key, or only let Enter activate a focus that was set by Tab. Make mouse clicks "select" without taking keyboard focus.

### [S2] Stock action sheet is taller than a 30-row terminal, with no scrolling, so the last actions and the cursor go off-screen
- Location: src/ui/ActionRegistry.cpp:89-142 (action list), :400-444 (render), src/app/shell/AppShellRender.cpp:113-119
- Category: ui-parity / layout (supported minimum 100x30)
- Failure scenario: with a part selected, the Stock registry yields 23 actions in 10 group runs (group headers repeat: System, Search and System again). The sheet is
  23 + 10 headers + title + divider = 35 rows. The frame is header 1 + divider 1 + page (flex, shrinks to 0) + divider 1 + sheet 35 + message 1 = 39 rows, against 30. About 9 rows overflow at the bottom of the terminal.
  FTXUI clips the overflow. The `Data` group (export, backup, retry save) and `quit` are invisible and cannot be clicked, and `handleActionSheetKey` moves `sheetIndex_` down into the invisible rows with no scrolling and no visible cursor. The same overflow occurs at 120x30 because only the height matters.
  Racks sheet fits at exactly 29 rows; the Stock sheet with a selection (the common case) does not.
- Evidence:
  ```
  rows.push_back(fullLine("  Inventatory actions ...", ...));
  rows.push_back(uiDivider());
  for (size_t index = 0; index < sheetActions_.size(); ++index) { ...one row per action + one per group change... }
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, screenWidth);
  ```
- Confidence: medium-high (computed row counts from the code; clipping behaviour from FTXUI semantics, no capture).
- Proposed test: a pure function that returns the sheet row count for a given action list, asserted at most (terminal rows - 5) for every page at 30 rows.
  Failing that, a documented 100x30 capture checklist item.
- Proposed fix (sketch): window the sheet (`yframe` plus `select` on `sheetIndex_`), or cap its height and scroll. Merge repeated group runs so headers appear once. Optionally two columns at wider widths.

### [S2] Projects: "Build complete . stock updated . Ctrl+Z undoes it" is false on that page; Ctrl+Z is not wired on Projects
- Location: src/app/bom/AppBomBuildActions.cpp:197, src/ui/ActionRegistry.cpp:295-339 (no CtrlZ action), src/ui/pages/bom/BomProjectPage.cpp (no CtrlZ branch); docs/user-guide.md:103
- Category: ui-parity / correctness
- Failure scenario: the user finishes "Find in racks" and answers `y` (subtract). The message tells them Ctrl+Z undoes it, and the user guide repeats the promise. The user stays on Projects (`bomView_ = Split`) and presses Ctrl+Z. `dispatchAction` finds no CtrlZ trigger for Projects, `handleBomProjectKey` ignores it, and nothing happens, with no message. Undo only exists on Stock, Racks and History. The safety net for a bulk stock deduction silently does not work where the promise is made.
- Evidence:
  ```
  setMessage(subtractFromStock ? "Build complete · stock updated · Ctrl+Z undoes it" : ...)   // stays on Page::Projects
  ```
  Projects `case` in `currentActions()` contains no `KeyType::CtrlZ` entry (Stock/Racks/History do).
- Confidence: high.
- Proposed test: for each page that tells the user "Ctrl+Z", assert `currentActions()` contains the CtrlZ trigger.
- Proposed fix (sketch): add `add("undo", "System", "Ctrl+Z", special(KeyType::CtrlZ), ...)` to the Projects registry (and Import where it makes sense). Reword the message otherwise.

### [S3] UTF-8 display width: `ellipsize`, `wrapText` and the hard-wrap helpers are byte-based
- Location: src/ui/shared/AppUiShared.cpp:369-377 (`ellipsize`), :379-410 (`wrapText`); src/ui/pages/racks/RackManagementPage.cpp:127-131 and :154-156; src/ui/pages/bom/BomProjectPageRender.cpp:163-167; src/ui/shared/AppUiItemDetails.cpp:422 (`field.label.size()`); StockPage.cpp:41-43 (longest-name sizing)
- Category: correctness (display), sibling of the already-merged text-input fix
- Failure scenario: all of these use `std::string::size()` (bytes) as the column count and cut with `substr(byte_offset)`.
  - `ellipsize` can cut inside a multi-byte sequence. "Kondensator 1000µF" is 18 bytes (µ is 2) for 17 cells. With maxLength 17 it becomes "Kondensator 100" plus "..." in practice, dropping a digit of the value. For other lengths it cuts exactly between the two bytes of µ, Ω, ±, ł or °, leaving a stray lead byte followed by "...". FTXUI drops or mangles that invalid sequence. The truncation always fires about one cell early for every non-ASCII char, and electronics part names are full of µ, Ω, ±, ° (and now, after the input fix, Polish letters).
  - Hard-wrapping a long word (`rackTitleLines`, `bomRackTitleLines`) uses `word.substr(offset, lineWidth)` at byte offsets. A long non-ASCII part name loses the characters split across lines in the rack grid and in the Find-in-racks grid (the orphan continuation bytes at the start of the next line are dropped).
  - `wrapText` (release notes, `itemDetailText`) neither splits an over-long word nor counts cells. Notes with "—", "→", "·" wrap early, and a long URL overflows the line.
  - Wide CJK and combining marks are not accounted for either (3 bytes for 2 cells; 2 code points for 1 cell).
- Evidence:
  ```
  if (maxLength == 0 || value.size() <= maxLength) return value;
  ...
  return value.substr(0, maxLength - 3) + "...";            // may split a UTF-8 sequence
  ...
  lines.push_back(word.substr(offset, static_cast<size_t>(lineWidth)));   // RackManagementPage.cpp:130
  ```
- Confidence: high that bytes are used; medium on how FTXUI renders the stray byte (it does not crash).
- Proposed test: `ellipsize("Kondensator 1000µF", 17)`, `ellipsize("Ωmega", 3)`, `ellipsize("日本語テキスト", 6)`. Assert the result is valid UTF-8 and its `ftxui::string_width` is at most the limit. Same for the hard wrap.
- Proposed fix (sketch): add `utf8Truncate(text, cells)` and `utf8Wrap(text, cells)` helpers in AppUiShared built on `ftxui::Utf8ToGlyphs` and `ftxui::string_width`. Use them everywhere the bytes are measured.

### [S3] Parameter lookup treats non-ASCII-only names as matching everything
- Location: src/ui/shared/AppUiItemDetails.cpp:15-35 (`normalizeKey`, `parameterLabelMatches`), `findParameter`:105-113
- Category: correctness (UTF-8 sibling)
- Failure scenario: `normalizeKey` keeps only `isalnum(unsigned char)` bytes, so a parameter name with no ASCII letters or digits normalises to "". `parameterLabelMatches(lhs, rhs)` returns true when `normalizedRhs.find(normalizedLhs) != npos`, and `"anything".find("")` is 0.
  DigiKey returns localised parameter names for `digiKeyLanguage` zh/ja/ko/ru/el/... (the setting is user-editable), and the edit UI now accepts any UTF-8 name.
  An item whose parameters include one fully non-ASCII name (for example "电容" or "Ω") then matches every lookup in `electricalFieldsForItem`, `packageSummary` in Racks and so on. The detail panel shows "Capacitance: <the value of that foreign parameter>" and "Package: <same>".
  Latin-with-diacritics names are only partially normalised ("Pojemność" becomes "pojemno") and can mismatch.
- Evidence:
  ```
  return normalizedLhs == normalizedRhs || normalizedLhs.find(normalizedRhs) != string::npos ||
         normalizedRhs.find(normalizedLhs) != string::npos;     // empty normalizedLhs => true
  ```
- Confidence: medium (the localised-import data path was not run).
- Proposed test: `parameterLabelMatches("电容", "Package")` must be false; `findParameter` on an item with only a CJK parameter must return null for "Capacitance".
- Proposed fix (sketch): reject empty normalised keys, and normalise by code point (or compare the trimmed, case-folded UTF-8 strings).

### [S3] Hidden accelerators and registry gaps: whole panels have no action-sheet entries
- Location: src/ui/ActionRegistry.cpp:190-261 (no `QuickLabels` case) vs src/ui/pages/settings/SettingsPageInput.cpp:77-95; src/ui/pages/stock/StockPageInput.cpp:136-146 and :162-251; src/ui/pages/import/ImportCsvPage.cpp:338-383
- Category: ui-parity (AGENTS.md: actions in the sheet and actions by direct key must come from the same `currentActions()`; docs/ui-contributor-guide.md: "Do not duplicate the same accelerator in a page key switch")
- Failure scenario: keys that work but are not in the sheet, so they cannot be discovered:
  - Settings > Quick Labels registers nothing. The sheet shows only "quit" (and save/discard when dirty), yet `a` add, `x` remove, `[` `]` move, `t` test and `e` edit are live. Settings > Scan and DigiKey get `e` (edit field), and the Printer panel gets `t` when no queue is selected, from the page handler only.
  - Import review: Enter = Accept and Backspace = Skip (the primary actions) are not in the sheet; it lists only "edit row" and "cancel import".
  - Stock: `u` (undo) and the upper-case aliases (`E`, `N`, `M`, `D`, `O`, `G`) work through the page handler's `tolower` but are not listed. Stock `+`/`-` have no `=`/`_` alias although Projects does.
  - Projects Build: Esc (leave Find in racks) and Left/Right are unlisted.
  Also the handler duplicates registry keys; for example the Stock 'r' reload body at StockPageInput.cpp:207-244 is a second, divergent copy of `reloadInventoryState()` that is unreachable because the registry's 'r' wins.
- Confidence: high.
- Proposed test: for each page/category, enumerate keys handled by the page handler and assert each has a `currentActions()` entry (or is on an explicit navigation allow-list).
- Proposed fix (sketch): register the missing actions (Quick Labels, Import accept/skip, Scan/DigiKey edit); delete the duplicated handler branches.

### [S3] Delete confirmations are keyboard-only popups, and the controls behind them stay clickable
- Location: src/ui/pages/stock/StockPage.cpp:259-298, src/ui/pages/racks/RackManagementPage.cpp:457-505
- Category: ui-parity
- Failure scenario: "Delete item" and "Delete rack" arm a timed popup that is confirmed only with Enter and cancelled with Esc (or any other key). The popup contains no `App::target`, so a mouse user can arm Delete but not confirm it.
  The popup is painted with `dbox` over the page, but its pixels are not hit-testable. A click on the visible popup falls through to whatever target lies underneath: `stock.increment`, `stock.print`, a list row. That changes quantity or selection while the delete confirmation stays armed on the old item.
- Evidence: `popupRows` contains only `paragraphAlignLeft(...)` text; `return ftxui::dbox({page, overlay});`
- Confidence: high.
- Proposed test: with the delete popup active, assert that the targets list contains confirm/cancel targets registered after the page's targets, and that the page's targets are disabled (`enabled=false`) while modal.
- Proposed fix (sketch): add Confirm/Cancel buttons (disabled until the timer unlocks) and register a full-popup "backdrop" target that swallows clicks.

### [S3] Clicking a navigation tab while the "unsaved settings" prompt is showing bypasses it and leaves the draft active
- Location: src/app/racks/AppRackActions.cpp:41 (`inputMode_ != InputMode::ExitConfirmation`), :47-48
- Category: correctness / ui-parity
- Failure scenario: leaving dirty Settings sets `inputMode_ = ExitConfirmation` (modal, keyboard S/D/Esc, with no mouse targets). A mouse user who then clicks another navigation tab hits the guard `page_ == Settings && settingsDirty_ && inputMode_ != ExitConfirmation`, which is false, so `changePage` proceeds: `page_` changes, `inputMode_` is reset, `pendingPageAfterSettings_` is left stale. The staged draft (data folder, DigiKey secret, appearance) is still dirty and invisible on the other page, and the live appearance preview applied by `applyUiAppearance(settingsDraft_...)` keeps painting the whole UI with unsaved colours. The next `q` re-opens the prompt.
- Confidence: high.
- Proposed test: drive `changePage` twice (dirty Settings, then nav click while `ExitConfirmation`) and assert the page did not change.
- Proposed fix (sketch): in `changePage`, treat `ExitConfirmation` as a block unless the caller is `completeSettingsExit` (pass a flag instead of testing the mode). Give the prompt clickable Save/Discard/Stay buttons.

### [S3] Leaving a setup wizard with a mouse click leaves the Wi-Fi password, secrets, BLE discovery and draft state behind
- Location: src/ui/pages/scanner/InventatoryScanSetupPage.cpp:305-318 (only the Esc path clears), src/ui/pages/digikey/DigiKeySetupPage.cpp:70-80, src/app/racks/AppRackActions.cpp:29-73 (`changePage`), src/app/shell/AppInput.cpp:113-121
- Category: security (secret handling) / correctness
- Failure scenario: the wizards swallow all keys, so the header navigation is the only way out other than Esc, and it is clickable. A click on any nav tab calls `changePage`, which does none of the wizard cleanup:
  - The Scan R1 setup keeps `bleWifiPassword_`, `bleWifiSsid_`, `blePairingCode_` and `inputBuffer_` (which holds the typed password on the WifiPassword step). `bleProvisioning_.stopDiscovery()` is never called, so BLE scanning continues in the background.
  - The DigiKey setup keeps `stagedDigiKeySecret_`, `stagedDigiKeySecretChanged_`, the `settingsDirty_` draft and `inputBuffer_` (holding the secret on the ClientSecret step). Returning to Settings shows "Unsaved changes" with a staged secret, and Save would store it.
  - `settingsEditingField_` and `appearancePickerOpen_` are not cleared by `changePage` either, so leaving Settings mid-edit and returning resumes the stale editor (and `1`-`6` then type into it).
  AGENTS.md requires releasing secrets on every path.
- Confidence: high.
- Proposed test: `changePage(Stock)` while `page_ == ScanSetup` and `bleWifiPassword_` non-empty; assert the password is wiped and discovery stopped.
- Proposed fix (sketch): add `leavePage(from)` hooks run by `changePage` (wipe wizard secrets, stop BLE, reset editors), and have the wizard `cancel` lambdas share it.

### [S3] Scan setup tells the user to "press R to retry saving", but R is swallowed by the wizard
- Location: src/ui/pages/scanner/InventatoryScanSetupPage.cpp:294-299, src/app/shell/AppInput.cpp:113-116 (wizard consumes all keys before the `R` retry handler at :132)
- Category: correctness (misleading recovery path)
- Failure scenario: when provisioning succeeded but the pairing data could not be saved, the message is "Scanner setup was sent, but pairing data is not fully saved; press R to retry saving". The wizard stays on the Confirm step, and in that step `R` is only handled in FindScanner. `R` does nothing. Pressing Enter again runs the whole provisioning again with the Wi-Fi password and code already wiped (`bleWifiPassword_`/`blePairingCode_` cleared at :287-289), which fails validation. The user must Esc out, and only then does `R` (global retry) work.
- Confidence: high.
- Proposed test: unit-level: after a failed `saveScannerConfigChecked`, the key `R` on `ScanSetupStep::Confirm` must trigger `retrySaveState`.
- Proposed fix (sketch): handle `R` in the wizard when `scannerConfigSavePending_ || scannerCredentialSavePending_`, or change the message to "press Esc, then R".

### [S3] DigiKey panel: pressing Enter on the blank "Client secret" field stages an empty secret and Save overwrites the stored one
- Location: src/ui/pages/settings/SettingsPageState.cpp:169-182 (`beginSettingsFieldEdit` case 1) and :237-240 (`commitSettingsFieldEdit`), src/ui/pages/settings/SettingsPageSave.cpp:129
- Category: correctness (credential data loss)
- Failure scenario: the Client secret row shows "••••••••". Clicking it (or selecting it and pressing `e`) opens an editor with an empty buffer. Pressing Enter without typing commits `stagedDigiKeySecret_ = ""` and `stagedDigiKeySecretChanged_ = true`, so the panel becomes dirty. If the user then presses Save, `CredentialStore::write(kDigiKeySecretName, "")` replaces the stored secret with an empty one, and DigiKey stops working until the user finds the secret in the DigiKey portal again. The setup wizard guards this case (`trim(inputBuffer_).empty()` rejected); the panel does not.
- Evidence:
  ```
  case 1:
    stagedDigiKeySecret_ = inputBuffer_;     // no empty check
    stagedDigiKeySecretChanged_ = true;
  ```
- Confidence: high.
- Proposed test: commit an empty secret edit and assert the draft stays clean and a message is shown.
- Proposed fix (sketch): reject an empty secret in the panel (or treat Enter on empty as cancel); add an explicit "Clear secret" action if removal is wanted.

### [S3] Quick-label length limit is in bytes but the message says "characters"
- Location: src/ui/pages/settings/SettingsPageState.cpp:217-221
- Category: correctness (UTF-8 sibling)
- Failure scenario: `preset.size() > kQuickLabelPresetTextLimit` with the text "Quick labels must contain 1 to 24 characters". After the text-input fix a user can type Polish/accented labels, and a label of 13 characters such as "Zasilanie 12V" with several two-byte letters is rejected. The limit is in bytes, not characters. Other UI caps behave the same way (`inputBuffer_.size() >= 160` in history search/checkpoint).
- Confidence: high.
- Proposed test: a 24-code-point label of "ż" must be accepted (or the message must say bytes).
- Proposed fix (sketch): count code points (or display cells) for the limit; keep the byte cap only as a storage bound.

### [S3] Content below the fold is not reachable: the Stock detail panel and most Settings panels cannot be scrolled, and Tab focus does not scroll into view
- Location: src/ui/pages/stock/StockPage.cpp:245-250 (`yframe`, no `select`), src/ui/pages/stock/StockPageDetail.cpp:80-252, src/app/shell/AppInput.cpp:293-328 (wheel handler), :344-365 (`moveUiFocus`), :229-273 (`target`)
- Category: ui-parity / layout
- Failure scenario:
  - The Stock detail body is `vbox(detailRows) | yframe | vscroll_indicator`, but none of the non-edit rows carries `ftxui::select`, so the frame always shows from the top. The mouse wheel over Stock moves the list selection regardless of the pointer position, and no key scrolls the detail. For a DigiKey part (movements, 6-10 electrical rows, Details disclosure, Links, Notes), at 100x30 the lower sections (Links, Notes, even the Details disclosure) are clipped and unreachable. Notes can only be read by entering Edit.
  - Settings panels use `yframe` and rely on `select` for the highlighted row only. The wheel does nothing there except in Printer. A mouse-only user cannot reach the lower controls of a long panel (Quick Labels with up to 12 presets pushes the Custom label section and the "Print custom label" button off-screen; Appearance puts Edit color below four colour sections).
  - Tab moves `focusedTarget_` across every target, including every list row (nav + filter + all N rows before the detail buttons). The focus ring is drawn by colour only and never calls `ftxui::focus/select`, so focus can sit on an invisible control and Enter activates it unseen. AGENTS.md: "no hidden focus trap".
- Confidence: medium-high (FTXUI frames scroll only to a selected child; no capture).
- Proposed test: manual 100x30 capture with a part that has 30 parameters and notes; and a unit check that `moveUiFocus` ends with a visible target.
- Proposed fix (sketch): wheel handler dispatched by pointer location (the History page already does this); `select` the focused target so frames follow it; skip row targets in the Tab order (rows are arrow-navigated); give the detail panel PageUp/PageDown scroll.

### [S3] Find-in-racks grid at the supported widths clips the quantity and slot label
- Location: src/ui/pages/bom/BomProjectPageRender.cpp:196, :421-431, :455-462
- Category: layout (100x30 / 120x30)
- Failure scenario: the Build view derives `slotWidth` from `equalRackSlotWidth((gridWidth - 4), 5)`. At 100 columns `gridWidth = 55` and the slot is 10 cells wide; at 120 columns it is 14. Each cell's footer is `hbox({" Quantity:[N] " (14-15 cells), filler, slot label "A1 " (3 cells)})` sized `EQUAL cellWidth`. At 100 columns that is at least 17 cells in 10; at 120, 17-18 in 14. FTXUI cannot shrink text, so the footer is clipped. At 100x30 the quantity digits and slot label (the part the user is told to open) are cut off. The Racks page has width tiers for exactly this reason (`width < 10` / `< 14` variants in `rackQuantityIndicator`), the BOM view ignores them. The slot is only identified by the pulsing highlight. It needs about 160 columns to render fully.
- Confidence: medium (computed widths; no capture).
- Proposed test: a 100x30 and 120x30 capture of Find in racks with a lit slot.
- Proposed fix (sketch): reuse `rackQuantityIndicator` (tiered text) for the BOM grid.

### [S3] Import: Esc, `q` and any nav click discard the whole staged review with no confirmation
- Location: src/ui/pages/import/ImportCsvPage.cpp:350-353 and :380-383, src/app/racks/AppRackActions.cpp:38-40
- Category: correctness (loss of in-memory work)
- Failure scenario: reviewing a 300-row DigiKey CSV, one accidental Esc (documented as "move back one level"), `q`, a stray click on a header tab, or a bumped digit key `1`-`6` calls `changePage`, which runs `cancelImportSession()` on `importStageActive_`. The accept/skip/edit decisions on all reviewed rows are discarded with only a "CSV import cancelled" toast. Backspace (skip) also acts immediately with no undo. AGENTS.md says destructive actions require an explicit command and confirmation.
- Confidence: high.
- Proposed test: with `importStageActive_` and at least one accepted row, `changePage(Stock)` must not discard without a confirmation step.
- Proposed fix (sketch): when any row has been accepted/skipped, route the first Esc/nav to a "Discard N reviewed rows? Enter / Esc" prompt.

### [S3] Projects: after the last stop, Enter (the same key used to advance) means "subtract these parts from stock"
- Location: src/ui/pages/bom/BomProjectPage.cpp:22-26, src/app/bom/AppBomBuildActions.cpp:114-120 (`advanceBomBuild` opens the prompt)
- Category: correctness (destructive default on a repeat key)
- Failure scenario: the walkthrough is driven by Enter ("Next stop Enter"). Past the last stop `bomDeductPrompt_ = true`. In that prompt `Enter` calls `finishBomBuild(true)` (deduct stock) and `Esc` keeps stock. A key-repeat or double-tap of Enter on the final stop opens the question and answers it with the destructive option in one go, deducting the BOM from stock without the user ever seeing the question. The visible prompt advertises only `y` / `n`. (Undo exists, but see the Ctrl+Z finding.)
- Confidence: high.
- Proposed test: after `advanceBomBuild` opens the prompt, Enter must not deduct.
- Proposed fix (sketch): make `y`/`n` the only confirmation keys, and make Enter in the prompt do nothing (or "keep stock").

### [S3] The resize notice does not block input, and the wizards bypass it while depending on a 100-column lockup
- Location: src/app/shell/AppShellRender.cpp:88-109, src/app/shell/AppInput.cpp:39-224, src/ui/pages/onboarding/OnboardingPage.cpp:46 (`kLockupColumns = 100`)
- Category: ui-parity / layout
- Failure scenario: below 100x30 `renderUi` returns the "needs a terminal of at least 100 x 30" notice, but `handleKey` still routes every key to the hidden page. Digits switch pages, `q` quits, Enter with a stale `focusedTarget_` activates things the user cannot see, and Delete-armed or edit modes continue. Also Onboarding, the Update page and onboarding-driven Scan setup bypass the notice completely (the check comes after them) while the first-run wordmark is a fixed 100 columns wide. On the default 80x24 terminal, the fresh-install first screen draws a clipped wordmark. The documented contract (docs/ui-redesign.md:29-30) says smaller terminals show the notice.
- Confidence: medium-high.
- Proposed test: with `dimx = 80`, assert `handleKey(Character 'q')` does not set `running_ = false`.
- Proposed fix (sketch): a single `terminalTooSmall()` predicate that gates both render and `handleKey` (keep Esc/Ctrl+C), applied to wizards as well, or give the wordmark a compact variant.

### [S3] O(g^2) work per frame at 10 Hz: category "is last" scans, repeated `selectedItem()` sorts
- Location: src/ui/pages/stock/StockPageList.cpp:120-127 (`categoryIsLast`), src/ui/pages/bom/BomProjectPageRender.cpp:578-596 plus src/app/bom/AppBomProjectActions.cpp:32-39 (`bomComparisonCategory`), src/app/inventory/AppInventorySelection.cpp:130-205, :219-224
- Category: perf (constant CPU burn and lag)
- Failure scenario: the 100 ms ticker posts `Event::Custom`, and FTXUI redraws after every event, so `renderUi()` runs 10 times a second and rebuilds the whole element tree.
  - Stock grouped view: `categoryIsLast` loops over all following rows of the same category, calling `displayCategory()` (string allocations) each time. A category with g rows costs g^2/2 calls per frame; 1000 resistors is about 500k allocations per frame.
  - BOM compare view: `categoryIsLast` calls `bomComparisonCategory()` for every following row. That function does a linear `find_if` over the whole inventory and two `toLower` allocations. Cost per frame is about g^2/2 x (N items). A BOM with 120 same-category lines and 3000 stock items is on the order of 7000 scans x 3000 comparisons, about 100 ms per frame, i.e. the UI loop is saturated.
  - `selectedItem()`/`selectedIndex()`/`filteredIndices()` each call `stockSearchMatches()`, which re-ranks, filters and sorts the whole inventory (with string allocations in the comparator). The Stock page calls it 3-5 times per frame and more per key press.
- Confidence: medium (not benchmarked; complexity is clear from the code).
- Proposed test: a micro-benchmark with 3000 items / 1000-row category asserting render prep is under about 20 ms.
- Proposed fix (sketch): compute "is last in group" in one backward pass; cache `bomComparisonCategory` per match when the analysis is built; cache `stockSearchMatches()` per (query, sort, store revision) and invalidate on mutation.

## Quality (S4)

### [S4] Duplicate action IDs; IDs are derived from the key hint
- Location: src/ui/ActionRegistry.cpp:32-38
- Category: quality
- Failure scenario: `id = pageName + "." + group + "." + keyHint`, lowercased, with non-alphanumerics replaced by '.' and runs collapsed. So:
  - `+`/`-` collide as "stock.edit." (add one / remove one), "racks.slot." (part plus/minus one) and "projects.project." (more/fewer boards).
  - `[`/`]` collide as "racks.racks.".
  - `p`/`P` collide as "stock.print.p".
  The sheet's hover state is keyed on "action." + id, so hovering one row highlights both. Because the key hint is part of the ID, changing an accelerator changes the action's identity, which contradicts AGENTS.md ("preserve an existing action's identity when changing its wording").
- Confidence: high.
- Proposed test: assert all IDs from `currentActions()` are unique for every page state.
- Proposed fix (sketch): use an explicit stable id string per `add(...)` call.

### [S4] Duplicated and dead code
- Location: StockPageInput.cpp:207-244 (second copy of reload logic), :157-160 (unreachable `Tab` branch), RackManagementPage.cpp:526-546 (Tab/Enter branches shadowed by `handleKey`), HistoryPage.cpp:255-270, ImportCsvPage.cpp:338-356; unused helpers `statusTextBox`, `bulletLine`, `quantityBadge`, `renderParameters`, `uiDangerFlashBg`, `App::printerSummary`, `summaryLine`, `itemDetailText`, `beginSettingsEdit`, `renderDeviceDebugConsoleUi` (+ `kDebugWindowLines`), `openSelectedDetail` (sets `page_` directly); Racks compact (`screenWidth < 100`) layout is unreachable below the 100-column notice.
- Notes: `uiPanelLeftBg/RightBg/RowDarkBg/RowLightBg/uiRowSelectedBg/uiMutedColor/uiDimColor/uiTitleColor/uiInfoColor` are all aliases of 4-5 real roles, which makes the role names misleading (85 call sites).
  The `Danger flash background` appearance role is user-editable in Settings > Appearance but nothing consumes it.
- Confidence: high.

### [S4] Divider colour is used for readable text
- Location: ImportCsvPage.cpp:258 (the Enter/e/Backspace hint), HistoryPageRender.cpp:465, SettingsPage.cpp:152-156 and :228-230 (table headers, group labels), BomProjectPageRender.cpp:445/456, RackManagementPage.cpp:317/326
- Category: quality (semantic roles)
- Failure scenario: `uiDimColor()` is the Divider role (56,69,67), which is about 1.8:1 against Surface (20,25,24). The palette in docs/ui-redesign.md reserves it for "necessary separators" and assigns hints to Muted (140,150,144). The import key hint, History hint and Settings group labels are barely readable. Literal RGB is otherwise confined to the colour picker and the lockup blend, which is fine.
- Confidence: high.
- Proposed fix (sketch): use `uiMutedText()` for text; keep `uiDimColor()` for rules.

### [S4] Update release-notes scroll offset is unbounded
- Location: src/app/shell/AppInput.cpp:320-325 (wheel `++updateNotesScroll_`), src/app/AppUpdate.cpp (Down/PageDown `++updateNotesScroll_`); render clamps
- Category: quality
- Failure scenario: scrolling past the end only increments the state; the render clamps to the maximum. Each extra Down/wheel notch has to be undone by an Up before the text visibly moves up. The Update wizard also has no Esc in `Preparing` (up to the 8-15 s network timeouts).
- Confidence: high.
- Proposed fix (sketch): clamp the offset when it is changed, using the same line count as the render.

### [S4] Smaller input/navigation inconsistencies
- Location and detail:
  - AppInput.cpp:356-358: Shift+Tab with no current focus lands on `n-2`, skipping the last target.
  - SettingsPageInput.cpp:114-117: the Left arrow moves to the previous category without `refreshPrinterState()`, unlike Up/Right/Down.
  - SettingsPageInput.cpp:137-139: a single Esc discards all staged settings (data folder, secret, colours) with no prompt.
  - AppInventoryEdit.cpp:30-39 and :41-47, AppRackActions.cpp:75-80: `startSearch`, `startClosestSearch`, `beginEditCurrentItem` and `openSelectedDetail` assign `page_ = Page::Stock` directly, bypassing `changePage` (the Racks `movingRackItemId_` is left set, the stocktake/import guards are skipped).
  - BomProjectPageRender.cpp:340: the Build view indexes `steps[stepIndex]` without checking `steps.empty()`; it is safe only because `beginBomBuild` checked once earlier.
  - OnboardingPage.cpp:404-407: Esc on any step marks onboarding complete (skips background/R1 choices) with no hint; the wizards (onboarding, Scan setup, DigiKey setup) expose no click targets, so a mouse-only user cannot continue.
  - AppUiItemDetails.cpp:285 and :360: parameter-name literals contain mojibake ("25Â°C", "10/1000Âµs"). They match only because `normalizeKey` drops all non-ASCII bytes.
  - SettingsPagePrivate.h `pickerCoordinates`: `round(max*6 - 1)` does not invert `pickerValue()` {0.30..1.0}, so opening the picker on an existing colour selects the wrong value row.
  - docs: ui-redesign.md says "persistent navigation and live system state", header status dots, and "no separate ... Inventatory Scan Setup page", but the header has only navigation and a clock, and Scan/DigiKey setup are separate pages. docs/ui-contributor-guide.md and AGENTS.md still say the shell and event dispatch live in `src/app.cpp`; it is `src/app/shell/`.

## Test gaps

- `ellipsize` / `wrapText` / hard-wrap helpers with µ, Ω, ±, ł, CJK, combining marks -> assert valid UTF-8 and `string_width <= limit`.
- `currentActions()` for every page/mode (needs a constructible App or a free function) -> unique ids, every handler key present, sheet height <= rows - 5 at 30 rows.
- Mixed mouse/keyboard focus: click a row, arrow, Enter on Import/Racks/Stocktake -> Enter action runs, selection does not move back.
- `changePage` guards: during `ExitConfirmation`, while a wizard is active (secrets wiped), with `importStageActive_` (confirmation), with `settingsEditingField_`.
- Input-mode echo table: every `InputMode` that edits `inputBuffer_` has a prompt (catches `BomRestock`).
- Render with more than 512 targets (ASan) -> `uiTargets_` bounds are valid.
- `parameterLabelMatches` with an empty normalised key.
- DigiKey panel: empty secret commit must not dirty the draft; quick-label limit counts characters.
- `terminalTooSmall` gating of `handleKey`.
- Projects Ctrl+Z after `finishBomBuild(true)`.
- inventatory_tests.cpp covers only the pure helpers in history_page_detail, app_navigation, stockFilterMenu* and rack_page_detail; nothing in ActionRegistry, `translateEvent`, the key handlers, `App::target` or `handleMouse`.
