# Inventatory: bug and vulnerability report

**Status:** All twelve reviewers have reported. Items marked **Reported** have not been reproduced and need a check before anyone acts on them.
**Commit reviewed:** `71bd158` on branch `claude/dazzling-meitner-4w6cy0`.
**Method:** Static, read-only review by twelve subagents split by area. No build and no test run, so there are no runtime results. Items marked **Verified** were re-read against the source by the orchestrator. Items marked **Reported** come from a reviewer's trace and still need a check before anyone acts on them.

**Totals:** 4 High, 15 Medium, 32 Low (51 findings).

## Summary

| ID | Severity | Status | Title |
|---|---|---|---|
| BUG-01 | High | Verified | Retrying a failed BOM build save deducts stock a second time |
| BUG-02 | High | Verified (mechanism) | Rack-slot print, Enter and detail act on the wrong Stock part |
| BUG-03 | High | Verified (scoring) | DigiKey keyword fallback accepts an unrelated product and merges its data |
| BUG-30 | High | Reported (design gap) | Update and bootstrap trust rests on one mutable GitHub release with no signature |
| BUG-04 | Medium | Verified | Mouse "Discard" in Settings skips the two-press confirmation |
| BUG-05 | Medium | Verified | Long DigiKey site, language or currency value makes every settings save fail |
| BUG-06 | Medium | Verified (script) | Bootstrap installer runs a downloaded PowerShell script without a checksum |
| BUG-07 | Medium | Reported | Background-service start failure leaves printer and secret state half applied |
| BUG-08 | Medium | Reported | Recovery rollback after a data-folder switch keeps the new DigiKey secret and autostart entry |
| BUG-09 | Medium | Reported | Regex recursion can overflow the stack on a long digit run in imported text |
| BUG-10 | Medium | Reported | Quadratic label text fitting can stall the label printer worker |
| BUG-11 | Medium | Reported | A 25 MB import expands to roughly 800 MB of parsed cells on the UI thread |
| BUG-12 | Medium | Reported | BOM analysis re-tokenizes every inventory description on the UI thread |
| BUG-21 | Medium | Reported (traced) | A `\u0000` escape in a DigiKey string can make the workspace unloadable |
| BUG-34 | Medium | Verified (call path) | Scan R1 BLE provisioning runs on the UI thread, and the Windows waits can hang |
| BUG-35 | Medium | Reported | A timed-out BLE token write is treated as a failure, so the scanner may keep a token the desktop discarded |
| BUG-38 | Medium | Reported | BOM parser reads milliohm as megaohm |
| BUG-39 | Medium | Reported | BOM value matching cannot read the micro sign |
| BUG-40 | Medium | Verified (mechanism) | Rack filter `rack:` matches by substring, so R1 also matches R10 to R19 |
| BUG-13 | Low | Reported | Stocktake counts wider than the quantity column are clipped |
| BUG-14 | Low | Reported | Ranked stock header overflows and can clip the Filter button at 100 columns |
| BUG-15 | Low | Reported | Filter panel clicks on blank cells fall through to hidden stock rows |
| BUG-16 | Low | Reported | Settings port note says "applies on next launch" but saving restarts the bridge |
| BUG-17 | Low | Reported | Uninstaller prints success after silently failing to delete secrets or data |
| BUG-18 | Low | Reported | DigiKey client secret and input buffers are cleared but not overwritten |
| BUG-19 | Low | Reported | Scan enrichment failures are dropped without any message |
| BUG-20 | Low | Reported | Windows DigiKey transport keeps the default redirect policy |
| BUG-22 | Low | Reported | Escaped control characters from DigiKey reach display strings |
| BUG-23 | Low | Reported | "Test DigiKey" blocks the render thread for up to about 8 seconds |
| BUG-24 | Low | Reported | One oversized unused string fails the whole DigiKey lookup |
| BUG-25 | Low | Reported | A successful sync replaces the whole vendor metadata record |
| BUG-26 | Low | Reported | A closing quote followed by a space keeps the CSV parser in quoted mode |
| BUG-27 | Low | Reported | A failed BOM re-import invalidates the open project's analysis |
| BUG-28 | Low | Reported | DigiKey import conflict check is O(rows x inventory) with allocations |
| BUG-29 | Low | Reported | Settings DigiKey secret and input buffer are not scrubbed after storage |
| BUG-31 | Low | Reported | Downloads follow redirects without a final-host check |
| BUG-32 | Low | Reported | Windows download ignores the file close result |
| BUG-33 | Low | Reported | A successful Windows install can be reported as a failed update |
| BUG-36 | Low | Reported (one reviewer rated Medium) | Unauthenticated LAN clients can fill the pending-connection queue and block Scan R1 sync |
| BUG-37 | Low | Reported | Backup-bundle validator ignores directory enumeration errors |
| BUG-41 | Low | Reported | Bare numeric `param:` value falls back to substring match |
| BUG-42 | Low | Reported | Merge clamps a quantity delta to zero with no notice |
| BUG-43 | Low | Reported | Windows printer probe reports a ready queue when the status query fails |
| BUG-44 | Low | Reported | A failed Windows document close is reported as a successful print |
| BUG-45 | Low | Reported | Windows mDNS advertises the scanner on every network interface |
| BUG-46 | Low | Reported | BLE status callback can touch stack objects after `provision()` returns |
| BUG-47 | Low | Reported | In-flight printer results are discarded at shutdown or workspace change |
| BUG-48 | Low | Reported (low confidence) | HTTP reader wakeup can be lost during stop, which could hang shutdown |
| BUG-49 | Low | Verified (code) | BLE discovery list silently drops new devices at 256 entries, so a spoofed flood can hide the real scanner |
| BUG-50 | Low | Reported (hardening, low confidence) | BlueZ agent methods do not check the D-Bus sender |
| BUG-51 | Low | Reported (low confidence) | Advertised BLE device name reaches the terminal without control-character filtering |

---

## High

### BUG-01: Retrying a failed BOM build save deducts stock a second time
- **Verified.** The stock subtraction runs in memory before the save, and the save-failure branch returns early.
- **Location:** `src/app/bom/AppBomBuildActions.cpp:103-123` (subtract, then `saveState` fails and returns at about line 120). `bomDeductPrompt_` is cleared only after a successful save, at line 147. `src/ui/ActionRegistry.cpp:312-313` keeps the "y" binding live while the prompt is set. `src/core/bom/BomPickPlan.cpp:46-47` recomputes picks from the already-reduced quantity.
- **Trigger:** A project needs 5 of part X and stock X is 10. Press "y" while the save fails (disk full or file locked). Stock X is now 5 and the message says to press R. Press "y" again: the plan takes `min(5, 5) = 5` and stock drops to 0. Ten pieces leave stock for a five-piece build. Ctrl+Z reverts only the second deduction.
- **Fix:** Record that the subtraction has already been applied, and make a retry re-save only. Or revert the in-memory subtraction and clear `bomDeductPrompt_` on save failure.

### BUG-02: Rack-slot print, Enter and detail act on the wrong Stock part
- **Verified (mechanism).** Rack code stores a `store_.items()` index in `selectedPosition_`. `selectedIndex()` treats that value as a position in the sorted and filtered `stockSearchMatches()` list.
- **Location:** `src/app/racks/AppRackPrintingActions.cpp:29-35` (`printSelectedRackPartLabel`). `src/app/racks/AppRackActions.cpp:443` (`openSelectedRackItemDetail`, which resets the search but not the sort or date filter). `src/app/inventory/AppInventorySelection.cpp:193-200` (`selectedIndex`). `src/App.h:879` (default sort is `StockSortOrder::Az`). `src/ui/ActionRegistry.cpp:156` ("p") and `:159` (Enter).
- **Trigger:** Store order is A = "Resistor 10k", then B = "Capacitor 100nF". The default A-Z sort gives `[B, A]`. Press "p" on A's slot: `selectedPosition_ = 0`, and `selectedItem()` returns B. The label printed is for the capacitor. Enter on A's slot shows B in the detail pane, and "-" or Ctrl+Backspace then changes B.
- **Fix:** Never store a store index in `selectedPosition_`. Find the item's position in `stockSearchMatches()`, or key the selection by item id. Reset the Stock sort and date filters the same way the search is reset, so the target is visible.

### BUG-03: DigiKey keyword fallback accepts an unrelated product and merges its data
- **Verified (scoring).** After a direct lookup fails, the client runs a keyword search and accepts the best result whenever its score is above zero. Tokens of two or more characters match by substring and add points. Nothing checks that the returned product number or manufacturer part number equals the requested one.
- **Location:** `src/platform/digikey/DigiKeyApi.cpp:296-316` (fallback after direct lookup). `src/platform/digikey/DigiKeyProduct.cpp:177-216` (`scoreSearchMatch`: a substring token match adds +8, with no identity requirement). `:252-277` (`resolveSearchResult` accepts any `bestScore > 0`). Reviewer-reported call sites: `src/app/import/AppImportActions.cpp:298-343` (CSV sync) and `src/app/scanner/AppDeviceActions.cpp:189, 221` (scan of a new code).
- **Trigger (reviewer-traced):** A CSV sync runs on an item whose part number is discontinued, so the direct call returns 404. The keyword search runs on that string. A token such as "nd" is a substring of many descriptions, so an unrelated product scores above zero and is accepted. The merge then overwrites parameters, unit price and vendor metadata. For a new scanned code with an empty part number, the unrelated product number is stored, and later syncs keep using it.
- **Fix:** Accept a keyword match only when its product number or manufacturer part number equals the requested identifier after normalisation. Otherwise return NoMatch and leave the item unchanged.

### BUG-30: Update and bootstrap trust rests on one mutable GitHub release with no signature
- **Reported (design gap).** The update manifest (`SHA256SUMS.txt` or `SHA256SUMS-linux.txt`) is downloaded from the same GitHub release as the archive and installer it covers. It has no signature, attestation or pinned key. Anyone who can replace release assets can replace the manifest too. The only "immutability" check is a one-time existence test before creation. Its message says "immutable", but nothing enforces that.
- **Location:** `src/platform/system/UpdateService.cpp:638-661` (`parseSha256Checksum`). `src/app/AppUpdate.cpp:296-330` (verification). `.github/workflows/release.yml:139-151` (manifest creation) and `:324-328` (existence check only).
- **Trigger:** An attacker with release write access (a stolen maintainer token, a compromised publish job, or a later `gh release upload --clobber`) replaces the archive, the installer and the manifest with a consistent set. The updater's hash checks pass, and the installer runs as the user: `sh` on Linux, or `powershell -ExecutionPolicy Bypass` on Windows.
- **Caveat:** Exploitation requires compromise of the release itself. This is a trust-model weakness, not a bypass of the current checks.
- **Fix:** Sign the manifest (for example with minisign, or Sigstore or `actions/attest-build-provenance`) using a public key compiled into the app, and verify the signature before any hash comparison. Also enable GitHub immutable releases.

---

## Medium

### BUG-04: Mouse "Discard" in Settings skips the two-press confirmation
- **Verified.** The keyboard route (Esc) goes through `requestSettingsDiscard()`, which needs a second press. The mouse target calls `cancelSettingsDraft()` directly.
- **Location:** `src/ui/pages/settings/SettingsPage.cpp:739` (mouse target). `src/ui/ActionRegistry.cpp:276` and `src/ui/pages/settings/SettingsPageInput.cpp:90-101` (keyboard route).
- **Trigger:** Type a new DigiKey client secret, change the site, then click "Discard". All staged edits are dropped at once, and the staged secret cannot be recovered.
- **Fix:** Have the mouse target call `requestSettingsDiscard()`, so both routes share the same confirmation.

### BUG-05: Long DigiKey site, language or currency value makes every settings save fail
- **Verified.** The editor accepts any length, but the validator caps these fields at 32 bytes. The save then fails with a generic message, and the draft stays dirty.
- **Location:** `src/ui/pages/settings/SettingsPageState.cpp:247-250` (no length check at commit). `src/app/settings/AppSettings.cpp:30` (`kMaxLocaleFieldBytes = 32`), `:69-71` (save validation), `:277` (load validation).
- **Trigger:** Enter a 40-character site value and press "s". The save fails, and every later save fails the same way until the value is removed.
- **Fix:** Apply the same byte limits at commit time and show a field-specific message, so an invalid draft is rejected before activation.

### BUG-06: Bootstrap installer runs a downloaded PowerShell script without a checksum
- **Verified (script).** The CMD bootstrap downloads `Install-Inventatory.ps1` with `curl` and runs it with `-ExecutionPolicy Bypass`. It never fetches or checks `SHA256SUMS.txt`, even though the release lists that script as a checksum asset. The zip check runs inside the script, so a substituted script skips it.
- **Location:** `installer/Install-Inventatory.cmd:8-11`. `.github/workflows/release.yml:128-151` (bootstrap URL rewrite and manifest).
- **Trigger:** The release, or a redirect target, serves a different `Install-Inventatory.ps1`. The user runs the published CMD, and the tampered script runs in the user session. Exploitation needs release compromise or a bad origin response.
- **Fix:** Download `SHA256SUMS.txt` in the bootstrap, compare the `.ps1` hash, and abort on mismatch before running it. The signing fix in BUG-30 covers this too.

### BUG-07: Background-service start failure leaves printer and secret state half applied
- **Reported.** The function returns early when the background controller fails to start, before the printer queue is applied and before the post-save cleanup.
- **Location:** `src/ui/pages/settings/SettingsPageSave.cpp:263-287` (early return at 286). Skipped block at 292-313.
- **Trigger:** Change the printer queue and enable "Keep running when closed". `start()` returns false. The settings show the new printer, but labels still print to the old queue until restart. `stagedDigiKeySecretChanged_` stays true.
- **Fix:** Record the warning and fall through to the printer apply and cleanup, instead of returning early.

### BUG-08: Recovery rollback after a data-folder switch keeps the new DigiKey secret and autostart entry
- **Reported.** When `loadState()` sets `inventoryRecoveryRequired_`, the rollback restores settings and the workspace, but not the secret or the autostart entry.
- **Location:** `src/ui/pages/settings/SettingsPageSave.cpp:208-226` (rollback path), compared with `:116-123` (`rollbackDigiKeySecret`) and `:142-156` (startup change).
- **Trigger:** Choose a data folder whose BOM project table fails to load, and enter a new DigiKey secret. The save reports failure, but the credential store and the OS login entry keep the new values. Discard then leaves the store changed without any sign in the UI.
- **Fix:** Call `rollbackDigiKeySecret()` and re-apply `setBackgroundStartupEnabled(settings_.backgroundServiceEnabled)` on this path, as the other failure branches already do.

### BUG-09: Regex recursion can overflow the stack on a long digit run in imported text
- **Reported.** Confidence is medium: libstdc++ `std::regex` behaviour is inferred from the code, not run.
- **Location:** `src/core/parts/PhysicalValue.cpp:446-451` (`extractInductance`). Also `src/label_printer/layout/LabelPrinterText.cpp:36-41`, `src/ui/shared/AppUiItemDetails.cpp:94-97, 133-137`, and `src/import/digikey/DigiKeyCsvImport.cpp:274`.
- **Trigger:** Import a DigiKey CSV whose description is "INDUCTOR " followed by about 100,000 digits. Opening that item's detail or printing its label runs the regex, and the process can crash. The item is stored, so the crash repeats.
- **Fix:** Replace the regex with a bounded hand-written scanner for the same grammar, or reject long inputs before matching.

### BUG-10: Quadratic label text fitting can stall the label printer worker
- **Reported.** When no size fits, `fitFont0Text` removes one character at a time and re-measures each candidate. That is O(n²) in the name length, and ordinary items skip any length cap on this path.
- **Location:** `src/label_printer/layout/LabelPrinterTextLayout.cpp:251-263`. `src/label_printer/core/LabelPrinter.cpp:122, 151`. `src/label_printer/core/LabelPrinterDetails.cpp:241-242`. `src/app/shell/AppRuntime.cpp:264`.
- **Trigger:** Import a 100 KB description and print its label. The single worker stays busy and later jobs stall. Separately, `estimateFont0Width` accumulates an `int` that the caller multiplies by up to 40. About 700 KB of wide characters overflows `int`, which is undefined behaviour.
- **Fix:** Cap the text before fitting, or find the cut point by binary search. Compute widths in a 64-bit type.

### BUG-11: A 25 MB import expands to roughly 800 MB of parsed cells on the UI thread
- **Reported.** Limits apply per row, per field and per file, but not to total parsed memory. The file is also parsed twice (format detection, then import). KiCad designators are capped per row only.
- **Location:** `src/import/csv/CsvReader.cpp:20-22, 249-270`. `src/import/csv/CsvFormat.cpp:38`. `src/import/kicad/KicadBom.cpp:19, 58-80, 217`. `src/app/import/AppImportActions.cpp:58-63`.
- **Trigger:** A 25 MiB file of 51,200 lines, each with 511 commas, makes about 51,200 × 512 string objects, roughly 800 MiB, on the UI thread.
- **Fix:** Add a total cell or byte budget for the whole parse, and avoid a second full parse during format detection.

### BUG-12: BOM analysis re-tokenizes every inventory description on the UI thread
- **Reported.** `analyzeBom` scores every (BOM line, inventory item) pair. When the item has no matching parameter, the value is re-parsed from the item's name and notes each time, with no per-item cache. This runs on the UI thread on each refresh.
- **Location:** `src/core/bom/BomMatch.cpp:229-234`. `src/core/bom/BomMatchHelpers.cpp:199-247` (`tokenizeQuery` at 232). `src/app/bom/AppBomProjectActions.cpp:183-200`.
- **Trigger:** A DigiKey row with about 1 MiB of "10k " tokens, plus a KiCad BOM with a few hundred resistor lines. Each refresh takes seconds.
- **Fix:** Compute each item's candidate value and tokens once per analysis, or cap the description length used for value recovery.

### BUG-21: A `\u0000` escape in a DigiKey string can make the workspace unloadable
- **Reported (traced by the persistence reviewer; the trigger is not confirmed in practice).** The DigiKey decoder turns `\u0000` into a real NUL byte, and nothing downstream rejects it. Item text is bound to SQLite with `c_str()` and length -1, which stops at the first NUL. The serialized commit snapshot is cut mid-record. The next history validation fails, and the workspace reports "could not read the existing inventory database".
- **Location:** `src/platform/digikey/DigiKeyJsonParser.cpp:273-274` (NUL appended). `src/app/common/AppActionSupport.h:188` (vendor metadata copied unconditionally). `src/core/history/InventoryVersion.cpp:110, 127` (`bind_text` with -1). `src/core/storage/InventoryStorageSqlite.cpp:229`. `src/core/inventory/InventorySerialization.cpp:254-270`. `src/app/persistence/AppHistoryPersistence.cpp:37-40`. `src/core/storage/InventoryStorage.cpp:59`.
- **Trigger:** A DigiKey response contains `\u0000` in ProductDescription or DetailedDescription. The save reports success, because the write does not round-trip the snapshot. The next launch fails history validation. Backups also fail, because the same check runs on the staged copy. Recovery needs manual steps.
- **Fix:** Reject a zero codepoint in the DigiKey decoder, as the scanner decoder already does (`InventatoryScanProtocolJson.cpp:74`). Bind every text value with its explicit size rather than -1. Round-trip `deserializeItemStrict` before the commit row is written.

### BUG-34: Scan R1 BLE provisioning runs on the UI thread, and the Windows waits can hang
- **Verified (call path).** The Confirm step calls `provisionSelectedBleSetupDevice()` synchronously from the key handler, so the whole provisioning sequence runs on the render thread.
- **Location:** `src/ui/pages/scanner/InventatoryScanSetupPage.cpp:381` (call from the Confirm step), reached via `src/app/shell/AppInput.cpp:112, 193`. Windows: `src/platform/scanner/BleProvisioningService.cpp:163, 189, 221, 325-326` (unbounded `.get()` on WinRT async calls, including `PairAsync` and `WriteValueAsync`) and the 25 s `wait_for` at `:337`. Linux: `src/platform/scanner/BleProvisioningServiceLinux.cpp:857-858` (pair, 30 s), `:893-901` (up to 12 × 5.5 s), `:917-918`, `:941-943` (write, 15 s), `:951` (25 s wait).
- **Trigger:** The Windows BLE stack stops answering during `PairAsync` or `WriteValueAsync`. `.get()` never returns, and the UI, including Esc and shutdown, stops responding. On Linux, the reviewers' worst-case sums of the D-Bus timeouts range from about 95 seconds to about three minutes of frozen UI. The Linux BLE reviewer counted the Pair (30 s), Connect (15 s), up to 12 GATT attempts, StartNotify (8 s), WriteValue (15 s) and the 25 s notification wait (`BleProvisioningServiceLinux.cpp:857-958`).
- **Fix:** Run `provision()` on a worker future and deliver the outcome to the UI loop, as the update and DigiKey workflows already do. Bound each WinRT wait and add cancellation.

### BUG-35: A timed-out BLE token write is treated as a failure, so the scanner may keep a token the desktop discarded
- **Reported.** Confidence is medium because scanner firmware is not in the repo, so its behaviour after a missed reply is inferred.
- **Location:** `src/platform/scanner/BleProvisioningServiceLinux.cpp:941-943, 969-978`. The token is saved only when the outcome is not `Failed` (`src/ui/pages/scanner/InventatoryScanSetupPage.cpp:208-212`, verified). Same classification on Windows: `src/platform/scanner/BleProvisioningService.cpp:322-327, 365-374`.
- **Trigger:** Confirm is pressed. The scanner applies the write, but the BlueZ reply is delayed past 15 s and no CONNECTING notification arrives. `provision()` returns Failed, and the candidate token is never saved. The scanner now holds a token the desktop does not have, so the user must erase the device to re-pair.
- **Fix:** Treat a D-Bus timeout (`org.freedesktop.DBus.Error.NoReply`) or a write that threw as Indeterminate, and keep the candidate token as the existing Indeterminate branch does. Count only a definitive ATT error reply as rejection.

### BUG-38: BOM parser reads milliohm as megaohm
- **Reported.** For resistance notation, a lowercase `m` is treated as mega, because the code uses `resistanceLike ? 1e6 : 1e-3`. The sibling parser `PhysicalValue.cpp` (`parsePrefix`, around line 126) reads `m` as milli, so the two core parsers disagree.
- **Location:** `src/core/bom/BomMatchHelpers.cpp:91-135` (resistance flag at 95, multiplier around 133-135), called from `src/core/bom/BomMatch.cpp:138`.
- **Trigger:** A BOM line "10mOhm" (a current-sense resistor) parses as 10 MΩ. An inventory item with Resistance "10 MOhms" (a real 10 MΩ part) also parses as 10 MΩ, so it scores 90 and is offered as sufficient stock. Conversely, a BOM "0.01R" never matches an item "10 mOhms", so the line is reported short and the part is never picked.
- **Fix:** Make lowercase `m` milli in every notation, and keep only uppercase `M` as mega. Add BOM tests for "10mOhm" against "0.01R" and "10 MOhms".

### BUG-39: BOM value matching cannot read the micro sign
- **Reported (impact depends on the item name lacking an ASCII value, which DigiKey titles often do not have).** The multiplier scan treats any byte that is not an ASCII letter, digit or `.` as invalid, so the UTF-8 micro sign (`\xC2\xB5`) is rejected. "0.1 µF" never yields a value. `PhysicalValue.cpp` (`parseUnicodeMicroPrefix`, around line 136) handles this, but the BOM path does not.
- **Location:** `src/core/bom/BomMatchHelpers.cpp:98-107` (byte loop in `parseNumberWithMultiplier`), reached from `itemValueFor` at `:199-246`.
- **Trigger:** A BOM line "0.1uF" with footprint C_0603. The inventory item is named "Ceramic cap" and has DigiKey parameter Capacitance "0.1 µF". The parameter fails, the free text fails, and the BOM line is reported short although the part is stocked.
- **Fix:** Normalise U+00B5 and U+03BC to `u` before parsing, as `parseUnicodeMicroPrefix` does. Add a BOM test with a µF parameter and a name without a value.

### BUG-40: Rack filter `rack:` matches by substring, so R1 also matches R10 to R19
- **Verified (mechanism).** `rack:` runs a case-insensitive substring test on the full location string (`tokenMatchesField` → `containsInsensitive`), so a numeric rack code matches as a prefix of longer codes.
- **Location:** `src/core/query/InventoryQueryMatching.cpp:282-286` (`rack:` branch) and `:194-196` (`tokenMatchesField`). Rack string built in `src/core/racks/RackAllocation.cpp:226-230`.
- **Trigger:** A stock query `rack:r1` with items in R1-A1 and R10-A1. "r10-a1" contains "r1", so R10 parts are returned. `rack:r2` similarly returns R20 to R29.
- **Fix:** Match the rack code exactly, or the code followed by `-`, so `r1` matches only `R1-…`.

---

## Low

- **BUG-13: Stocktake counts clipped** (`src/ui/pages/stock/StockPage.cpp:41-48, 199-200`). The quantity column width comes only from saved quantities. A typed stocktake count can be up to 10 digits and is truncated in the list. Fix: include stocktake count strings in the width while stocktake is active.
- **BUG-14: Ranked header overflows at 100 columns** (`src/ui/pages/stock/StockPage.cpp:76, 80-119`; `StockFilterState.h:84-86`). The ranked header can take about 66 cells in a 57-cell list, pushing the Filter button out of the panel. The "f" key still works. Fix: shorten the header or reserve the Filter width first, and add the ranked case to the header-budget test.
- **BUG-15: Filter panel blank cells fall through to hidden rows** (`src/ui/pages/stock/StockPage.cpp:128-143, 275-336`; `src/app/shell/AppInput.cpp:269-341`). Only the option cells are targets, so a click on "Sort" selects a stock row behind the panel. Fix: make the whole panel a non-actionable but hit-consuming target.
- **BUG-16: Port note is wrong** (`src/ui/pages/settings/SettingsPage.cpp:459`; `SettingsPageSave.cpp:262`). The note says "applies on next launch", but saving restarts the bridge on the new port. Fix: remove the note or describe the restart.
- **BUG-17: Uninstaller reports success after silent failures** (`installer/Uninstall-Inventatory.ps1:65-76, 91-97`). `cmdkey` exit codes are not checked, `catch` cannot fire for native failures, and `Remove-Item` uses `SilentlyContinue`. The script still prints "Inventatory was removed.", so secrets and data can remain. Fix: check `$LASTEXITCODE`, use `-ErrorAction Stop`, and list anything left behind.
- **BUG-18: DigiKey secret buffers only cleared, not overwritten** (`src/ui/pages/digikey/DigiKeySetupPage.cpp:129-131, 74-80, 23-24`; `src/ui/pages/settings/SettingsPageState.cpp:244, 256`; `SettingsPageSave.cpp:308`). Hardening only. The codebase already overwrites with `assign('\0')` before `clear()` elsewhere. Fix: do the same on every exit path.
- **BUG-19: Scan enrichment failures are dropped** (`src/app/scanner/AppScanEnrichment.cpp:67-71, 85-90`). The worker discards the error, so a scan with DigiKey unreachable shows "completed" with a placeholder name and no message. Fix: carry `result.error` through and call `setMessage` or `logActivity`.
- **BUG-20: Windows DigiKey transport keeps the default redirect policy** (`src/platform/digikey/DigiKeyTransport.cpp:181-193`). The Linux transport disables redirects. The Windows request follows cross-host redirects and may resend the bearer token and client secret. Confidence is medium because WinHTTP behaviour was not run. Fix: set `WINHTTP_OPTION_REDIRECT_POLICY_NEVER`.
- **BUG-22: Escaped control characters reach display strings** (`src/platform/digikey/DigiKeyJsonParser.cpp:273-274`; `src/app/common/AppActionSupport.h:175-176`). Raw control bytes are rejected, but `\u001b` and similar escapes are accepted and can reach the terminal. Confidence is low because the FTXUI rendering path was not checked. Fix: reject or replace code points below 0x20 and 0x7F in DigiKey text.
- **BUG-23: "Test DigiKey" blocks the render thread for up to about 8 seconds** (`src/ui/pages/settings/SettingsPageState.cpp:127-128`). The HTTPS token request runs on the render thread, against the project's worker rule. Fix: run it in a future and show "testing".
- **BUG-24: One oversized unused string fails the whole DigiKey lookup** (`src/platform/digikey/DigiKeyJsonParser.cpp:181-186`; `DigiKeyApi.cpp:252-259`). Any string over 4 KiB, including unused fields, fails the document. Fix: skip or truncate unused oversized strings.
- **BUG-25: A successful sync replaces the whole vendor record** (`src/app/common/AppActionSupport.h:186-189`). `item.vendorMetadata = details.vendorMetadata` drops `categoryPath` and title when a later response omits them, which can change printed labels. Fix: fill empty fields from the new record.
- **BUG-26: CSV closing quote followed by a space** (`src/import/csv/CsvReader.cpp:219-243`, close test at 225-226). A quote closes a cell only when the next character is the delimiter or a line break. `"abc" ,` stays quoted, so later rows merge until the import fails with "unterminated quoted field". Fix: skip spaces and tabs before the delimiter check.
- **BUG-27: A failed BOM re-import invalidates the open project** (`src/app/bom/AppBomProjectActions.cpp:126-131, 183-186`). `bomFile_` is assigned before the success check, so the next refresh clears the analysis. Fix: parse into a local and assign only on success.
- **BUG-28: DigiKey import conflict check is O(rows × inventory)** (`src/import/digikey/DigiKeyCsvImport.cpp:228-254, 294`). Each row scans every item twice, with allocations on each comparison. A 100,000-row import can take minutes on the UI thread. Fix: build a lowercase hash map of existing part numbers once.
- **BUG-29: Settings DigiKey secret not scrubbed** (`src/ui/pages/settings/SettingsPageState.cpp:244, 256`; `SettingsPageSave.cpp:308`). Same as BUG-18 for the settings path. Hardening only.
- **BUG-31: Downloads follow redirects without a final-host check** (`src/platform/system/UpdateService.cpp:870-873` for libcurl with `FOLLOWLOCATION` and HTTPS-only redirects; `:698-708` for WinHTTP's default policy; `installer/Install-Inventatory.cmd:8` uses `-L`). After a redirect, any HTTPS host is accepted. The checksum comes from the same origin, so it does not show where the bytes came from. Hardening gap, not a bypass on its own. Fix: check the effective URL host (`CURLINFO_EFFECTIVE_URL` or `WINHTTP_OPTION_URL`) against an allowlist of GitHub hosts.
- **BUG-32: Windows download ignores the file close result** (`src/platform/system/UpdateService.cpp:755, 757-766`). `output.close()` is never checked, so a failed final flush (for example a full disk) can pass the size check. The failure then reads as "failed checksum verification". Nothing is installed, so this fails safe. Compare the Linux path at `:889-898`, which checks after close. Fix: check `output.fail()` after close and report "Could not write the update asset".
- **BUG-33: A successful Windows install can be reported as a failed update** (`installer/Install-Inventatory.ps1:293-299`; `src/app/AppUpdate.cpp:129-131`). The completion marker is written after activation and the backup is deleted. If the marker write fails, it is downgraded to a warning and the status stays "pending". On the next start the app says "The update did not finish", although the new version is installed, and the user may retry an update that already succeeded. Fix: write a distinct "activated" state, or retry the marker before deleting the backup.
- **BUG-36: Unauthenticated LAN clients can fill the pending-connection queue** (`src/platform/scanner/HttpServer.cpp:28-29, 129-142, 169-170, 226`). Any host on the private LAN can open 16 TCP connections (`kMaxQueuedClients`) and hold each for its 2 s deadline without sending a request. The accept gate then closes new connections on arrival, including authenticated Scan R1 sync requests. No data is exposed or changed, but the device keeps retrying. Fix: cap pending connections per source address, and give unauthenticated sockets a shorter header deadline. The Linux BLE reviewer rated this Medium (`src/platform/scanner/HttpServer.cpp:132, 170, 226-227`), because the flood keeps the Scan R1 sync refused for as long as it runs. It suggested 2 to 4 connections per IP and dropping idle pre-request sockets sooner.
- **BUG-37: Backup-bundle validator ignores directory enumeration errors** (`src/core/transfer/InventoryTransferValidation.cpp:31-36`). The error is checked only inside the loop body. If `directory_iterator` fails at construction, the loop ends without reporting it, and if `increment` fails mid-scan the scan ends early. The unlisted-file check is then skipped. Restore still copies only manifest-listed names and re-validates the staged copy, so the data impact is small. Fix: check `enumerationError` after the loop as well.
- **BUG-41: Bare numeric `param:` value falls back to substring match** (`src/core/query/InventoryQueryMatching.cpp:60-70`). A value with no unit, such as "10", is parsed as unknown, so the physical comparison is skipped and a plain substring test runs. A query for `param:Resistance=10` therefore returns a "100 Ohm" part, because "100 ohm" contains "10". Fix: when the needle is a bare number and the parameter is value-typed, compare numerically or require a token boundary.
- **BUG-42: Merge clamps a quantity delta to zero with no notice** (`src/core/inventory/InventoryMerge.cpp:127-135`, `ApplyDelta` at `:129-131`; caller `src/app/import/AppImportActions.cpp:134`). When the combined result is below zero, it is clamped to 0 silently, and the notice path runs only for the non-delta mode. Example: base 10, the CSV stages 5, and live stock is 2. The combined value is -3, which becomes 0 with no notice. Fix: add a notice when the clamp changes the result, in the style of the existing "quantity was changed elsewhere" notice.
- **BUG-43: Windows printer probe reports a ready queue when the status query fails** (`src/label_printer/platform/LabelPrinterWindows.cpp:212-218`; result used at `src/app/shell/AppRuntime.cpp:153` and `:260`). The first `GetPrinterW` call only sizes the buffer, and its return value is never checked. If the query fails, `bytesNeeded` stays 0 and the probe returns `ok = true` with "Printer queue opened". The failure appears later, during a real print. Fix: check the sizing call's return value, and return "Unable to query printer status" with `ok = false` unless the error is `ERROR_INSUFFICIENT_BUFFER`.
- **BUG-44: A failed Windows document close is reported as a successful print** (`src/label_printer/platform/LabelPrinterWindows.cpp:272-304`). The `endDoc` lambda calls `EndDocPrinter` and discards its result, and `success` is set before the document is closed. A failed close still returns true, so the label is reported printed. Fix: capture the `EndDocPrinter` result, make `success` depend on it, and set `*error` when it fails.
- **BUG-45: Windows mDNS advertises the scanner on every network interface** (`src/platform/scanner/MdnsService.cpp:292-297`, with `request.InterfaceIndex = 0` at `:294`; the Linux path restricts advertising at `:354-357`). Index 0 means all interfaces, so the private address and port are announced on any attached network, including a public Wi-Fi segment. Authentication still applies, so the impact is discovery disclosure, not access. Fix: register only on the interface index that owns the bound address, and fail closed if it cannot be resolved.
- **BUG-46: BLE status callback can touch stack objects after `provision()` returns** (`src/platform/scanner/BleProvisioningService.cpp:282-308, 344-349`). The `ValueChanged` lambda captures the mutex, condition variable and terminal flag from `provision()`'s stack by reference. `notify_one()` runs after the lock is released, and removing the event token does not wait for a handler already running. A late callback can therefore notify a destroyed condition variable. The window is narrow. Fix: hold the synchronisation state in a `shared_ptr` captured by value. The same pattern appears in the Linux BLE subscribe and unsubscribe callbacks (`src/platform/scanner/BleProvisioningServiceLinux.cpp:344-349, 386-390`, with waiters at `:353-357, 393-397`), where `notify_one()` runs after the mutex is released. Fix there: notify while still holding the mutex.
- **BUG-47: In-flight printer results are discarded at shutdown or workspace change** (`src/app/shell/AppRuntime.cpp:277-278, 296-319`; `src/app/shell/AppShell.cpp:134-141`; `src/ui/pages/settings/SettingsPageSave.cpp:126-128`). Printer work runs on a detached thread that shutdown never waits for. `stopPrinterWork()` marks the completion cancelled and drops the result. A label that was already sent to the printer gets no activity entry, and its quick-label record is rewritten to "failed / workspace_changed". Fix: keep a joinable handle or an in-flight count and wait for it within the shutdown deadline, or record the print result from the worker side before cancellation takes effect.
- **BUG-48: HTTP reader wakeup can be lost during stop, which could hang shutdown** (`src/platform/scanner/HttpServerLifecycle.cpp:75, 90`; `src/platform/scanner/HttpServer.cpp:165-166`). `stop()` clears `running_` and calls `notify_all()` without holding `pendingClientMutex_`. The reader can miss that notification and block in the condition variable, and `join()` then hangs. The worker queue avoids this because `stop()` takes its mutex first. Confidence is low, and the window is tens of nanoseconds. Fix: set `running_` and notify while holding `pendingClientMutex_`.
- **BUG-49: BLE discovery list drops new devices once full** (`src/platform/scanner/BleProvisioningServiceLinux.cpp:27, 673`; the only clear point is `startDiscovery()` at `:644`). `rememberDevice()` returns without storing a new address once 256 entries exist, and nothing evicts stale or spoofed entries. During the FindScanner step, a nearby device that advertises the setup UUID with rotating addresses and the name "Inventatory Scan R1" can fill the list, so the genuine scanner never appears until the user presses R. This is a setup denial of service and does not bypass pairing. Fix: expire entries not seen recently, and evict the oldest or weakest entry instead of dropping the new one.
- **BUG-50: BlueZ agent methods do not check the D-Bus sender** (`src/platform/scanner/BleProvisioningServiceLinux.cpp:433-462`). The agent is exported under the app's unique bus name. `methodCall()` checks the device path but never checks that the caller is `org.bluez`, and `Cancel()` takes no device argument and is accepted from any caller. If the system bus policy lets a local process send calls to this connection, that process could obtain the pairing code through `RequestPinCode` or abort pairing. The repository ships no D-Bus policy file, and the stock policy was not confirmed, so this is hardening. Fix: compare `g_dbus_method_invocation_get_sender()` with the BlueZ owner before handling any agent method.
- **BUG-51: Advertised BLE device name reaches the terminal unfiltered** (`src/platform/scanner/BleProvisioningServiceLinux.cpp:731-733`; rendered at `src/ui/pages/scanner/InventatoryScanSetupPageRender.cpp:53`). The name comes from the remote Alias or Name and is drawn as-is. Whether FTXUI passes ESC bytes through was not checked, and FTXUI is fetched rather than vendored. This needs a nearby device and a terminal that interprets the sequences. Fix: replace non-printable bytes in device names, and cap the length, before storing or rendering them.

---

## Checked and considered safe (reviewer-reported)

- **Scan R1 protocol:** the MAC covers method, path, device id, counter and the exact body. Header parsing rejects CR, LF and control bytes, and rejects duplicate headers, obs-fold, Transfer-Encoding and bad Content-Length. Token comparison is constant-time for equal lengths. The MAC is checked before JSON parsing and before counter reservation. Counters must strictly increase, and the replay marker is saved only after the durable callback succeeds. A save failure disables the listener.
- **Scan R1 listener:** binds only to RFC1918 or link-local IPv4 (and 127.0.0.1). Linux mDNS goes through Avahi on the bound interface. The device token is not written to `inventatory_scan.conf`, backups or logs. Backups exclude replay state, and restore regenerates the token.
- **Windows crypto and secrets:** BCrypt HMAC, key derivation and SHA-256 hashing check every return value and release handles on every path. `BCryptGenRandom` is checked, and an empty token is never saved. CredentialStore results are checked.
- **Windows process and network:** `CreateProcessW` uses mutable copies with correct argument quoting. Winsock start and cleanup are balanced, and the listener uses `SO_EXCLUSIVEADDRUSE`. `openUrl` accepts only HTTPS URLs and does not go through cmd.
- **SQL and transactions:** every value is bound. The only string-built SQL uses fixed identifiers and integer ranges. Transactions roll back on every failure branch, and statements are finalised.
- **Backups and restore:** names are whitelisted with no separators and no duplicates. Sizes are checked, SHA-256 is verified, and restore re-validates the staged copy before activation. The restore journal checks path ownership.
- **Atomic writes:** temp files use `O_EXCL` with mode 0600, are fsynced before rename, and the directory is synced afterwards. Settings and quick labels are size-capped with strict key sets.
- **Injection surfaces:** CSV export neutralises formula prefixes. The printer uses `execvp` with argv vectors and no shell. ZPL text is sanitised before every `^FD` field. CSV import rejects NUL.
- **DigiKey client:** product numbers are percent-encoded for path and query. Headers are checked for control characters. Linux libcurl keeps peer and host verification on and rejects non-HTTPS URLs. No TLS verification is disabled. The client secret is read only from the credential store and never logged. The JSON parser enforces depth, container, string and surrogate limits.
- **Updater:** downloads are limited to an allowlisted GitHub host and asset set. Checksum matching is exact and unique. Both installers verify the archive and themselves. The Linux installer validates every tar member. Update processes start with argv, not a shell. Version comparison rejects overflow and accepts only strictly newer releases.
- **Threading and shutdown:** the ticker thread is joined before the screen is destroyed. Async workers capture values or `shared_ptr`s. The device status queue is swapped under its mutex. Settings, restore and workspace switches stop the HTTP server before reassigning settings. Recovery mode blocks the shutdown save from overwriting a locked workspace.
- **Linux BLE and HTTP:** the BLE agent rejects unexpected device paths before setting the verified state. Wi-Fi SSID, password, token and pairing code are validated before any D-Bus call, and no error string or log includes them. HTTP caps headers at 8 KB and bodies at 64 KB, checks Content-Length for overflow, and rejects duplicate headers and Transfer-Encoding. Sockets use `SOCK_CLOEXEC`, so no helper process inherits them.
- **Terminal resize:** `terminalTooSmall` gates both rendering and input, so zero or small sizes never reach layout.
- **Core domain logic:** rack slot allocation, rack numbering, movement diffs, commit diffs and reversal, merge conflict notices and placement re-validation, PhysicalValue prefixes and notation, BOM demand aggregation and pick planning, and query quantity operators were traced and held up.
- **Stock and rack actions:** stock and rack delete are armed by ID and recheck the ID before erasing. History revert and restore confirmations are cancelled on every navigation path. `reloadInventoryState` refuses while there are unsaved changes.

## Coverage

**All twelve reviewers have reported.** Their findings are merged above. The Linux BLE and HTTP reviewer did not read `BleProvisioningService.h`, `MdnsService.h`, the SQLite writes in `acceptDeviceSyncEvents`, or the firmware-side GATT permissions, since the firmware is not in this repository.

**Known gaps across all reviewers:**
- `tests/terminal_input_parser_tests.cpp` tests FTXUI's upstream parser, which is not in the repo, so it was not reviewed.
- Windows code was read but not compiled or run. The Windows update path (`UpdateService.cpp`) was not fully read. One reviewer noted that `releaseVersion` reaches PowerShell `-File` without validation on Windows, but the Linux branch validates it. This is unverified.
- Several UI page input handlers were checked by grep and spot checks only. `src/ui/ActionRegistry.cpp` render-time lambdas were not reviewed in full.
- `src/app/UpdateWizardPresentation.h` and the wizard rendering in `src/app/AppUpdate.cpp` (lines 490-728) were checked by grep only.
- Not read in depth: `DigiKeyProduct*.cpp`, `DigiKeyJson.cpp`, the Windows CredentialStore, the Windows DnsService path in `MdnsService.cpp`, `HttpServerProtocol.cpp` header parsing, `BackgroundController.cpp` lines 1-155 (Windows), `AtomicFile.cpp` Windows retry path beyond line 200, `InventoryQueryMatching.cpp` tokenizer, `RackAllocation.cpp` lines 1-120 and 335-447, `AppRackActions.cpp` and `AppRackPlacementActions.cpp` in full, and `AppInventoryEdit.cpp` lines 1-240.
- No runtime testing was done. Nothing here has been reproduced.
