# Triage (orchestrator-verified)

| ID | Sev | Status | Finding | Location |
|---|---|---|---|---|
| T1 | S2 | CONFIRMED | Typed multi-byte UTF-8 (ł ą µ Ω) is truncated to its lead byte, so invalid UTF-8 goes into search, edit fields and the DB. Both platforms. | AppShell.cpp:94, Console.h:40 KeyEvent{char} |
| T2 | S2 | CONFIRMED | Linux mDNS never starts: avahi-publish-service has no --interface option | MdnsService.cpp:255-267 |
| T3 | S2 | CONFIRMED | Linux: disabling the background service in Settings releases the single-instance lock while the TUI keeps running. Windows stop() does not. | SettingsPageSave.cpp:290, BackgroundControllerLinux.cpp:377-387 |
| T4 | S2 | CONFIRMED (code) | Linux update download has a 15 s total transfer timeout for an asset of up to 512 MB, so it fails on slow links | UpdateService.cpp:846 |
| T5 | S2 | CONFIRMED (code) | No SOCK_CLOEXEC/accept4/pipe2. A browser started via xdg-open, or avahi, inherits the listen socket. | HttpServerLifecycle.cpp:491, HttpServer.cpp:98, ConsoleLinux.cpp:152 |
| T6 | S3 | CONFIRMED (code) | Any select() EINTR drops all pending scanner connections | HttpServer.cpp:209 |
| T7 | S3 | CONFIRMED (mechanism) | CredentialStore::read returns "missing" when the keyring is unavailable, so the settings rollback can erase the real DigiKey secret | SettingsPageSave.cpp:114,139..239; CredentialStoreLinux.cpp:47 |
| T8 | S2 | CONFIRMED | inventatory_input test is a no-op under NDEBUG (Release CI) | tests/terminal_input_parser_tests.cpp |
| T9 | S3 | to verify | /proc/self/exe " (deleted)" suffix reaches the unit/.desktop file and the fallback switches off the preference | 3 sites |
| …  | S3/S4 | unverified | remaining items in AB-linux-platform.md, I-shell-threading-update.md | |
| T1 | — | FIX READY | UTF-8 input fix on branch fix/utf8-text-input (worktree ../Inventatory-Software-utf8). Gemini implemented it, orchestrator reviewed it, and the GCC Release build plus ctest pass. Not committed. | |
| T10 | S1 | CONFIRMED (code) | Restore renames the whole data dir aside and remove_all's it. Unmanaged files (not on the 5-file allowlist) are lost, and they are not in the pre-restore backup either. | InventoryRestore.cpp:155,173; InventoryTransferRecovery.cpp:157 |
| T11 | S2 | CONFIRMED (code) | The HTTP worker reads printerService_.configuredPrinter() (a const string&) while the UI thread may assign it (string data race). Also reported by wave-1 agent I. | AppQuickLabelActions.cpp:133 |
| T12 | S2 | to verify | A persistent device-event commit failure spins the UI loop, grows the enrichment queue without bound, and blocks later events (head-of-line) | AppDeviceActions.cpp:127-217 |
| T13 | S3 | CONFIRMED (low impact) | Windows path equality and containment fallback (for not-yet-existing paths) lowercases byte by byte, so non-ASCII case differences (ŁÓDŹ vs łódź) compare unequal | InventoryTransferValidation.cpp:100,257 |
| — | — | CLEAN | Locale sweep (Gemini plus orchestrator spot-check): float parsing is centralized in parseClassicDecimal, streams keep the classic global locale (locale::global never called), all ctype calls pass unsigned char. No remaining siblings of b528bfb. | |

## D+F S2 verification (orchestrator, read-only code check on main @ 5ffa2f8, 2026-10-05)

| ID | Sev | Status | Finding | Evidence |
|---|---|---|---|---|
| T14 | S2 | CONFIRMED (code) | Scanner commit baseline is in-memory `store_`; no `hasPendingPersistence()` guard in `processDeviceSyncEvents`, so after a failed save the commit's change counts contradict its snapshots and `validateInventoryCommitHistory` fails ("recovery required"). `persistedStore_ = store_` also drops the pending draft. | AppDeviceActions.cpp:127-222, DeviceSyncStore.cpp:410-418, InventoryStorage.cpp:155-203 |
| T15 | S2 | CONFIRMED (code) | Edit overwrites scanner quantity changes (whole-record copy at begin-edit, whole-record assign on save; events drain regardless of inputMode_). | AppInventoryEdit.cpp:89,218 |
| T16 | S2 | CONFIRMED (code), not benchmarked | Full snapshot per commit; full history validation on load/save/navigation. Scalability, not correctness. | InventoryVersionHistory.cpp:196-258 |
| T17 | S2 | CONFIRMED (code; finding says executed) | parseRkmValue accepts p/n/u/m/k/g markers as resistance, so "4u7" and "M3" parse as Resistance. | PhysicalValue.cpp:212-266 |
| T18 | S2 | CONFIRMED (code) | itemValueFor free-text fallback; parseElectricalValue accepts bare numbers as ohms (comment BomMatch.cpp:117 says by design for designations), so "0603" etc. become resistances. Scoring end-to-end not re-traced. | BomMatchHelpers.cpp:215-219, BomMatch.cpp:117-123 |
| T19 | S2 | CONFIRMED (code) | Classifier failure writes Unassigned (same value as user choice); reconcile returns early for non-Automatic, so enrichment never racks the item. Intent unclear: S3 if "Unsorted stays unracked" is intended. | RackAllocation.cpp:178-189, AppScanEnrichment.cpp:98-103 |

## G+H S2 verification (orchestrator, main @ 0ce3761) - findings/GH-imports-labels.md (S2 4, S3 10, S4 7; S3/S4 not yet verified)

| ID | Sev | Status | Finding | Evidence |
|---|---|---|---|---|
| T20 | S2 | CONFIRMED (code) | Import review snapshots `store_` at begin and assigns it back at commit; scanner events keep draining (same missing guard as T14/T15), so scanner qty changes and scanner-created items are lost. Third instance of one root cause: whole-store/whole-record overwrite with no conflict check. | AppImportActions.cpp:65-67,122 |
| T21 | S2 | CONFIRMED (code; CUPS strings from knowledge, not run) | Linux `probePrinter` looks for " is "; a disabled queue prints "disabled since ...", so it reports "available". `enumeratePrinters` handles it, so the two paths disagree. | LabelPrinterLinux.cpp:168-182 |
| T22 | S2 | CONFIRMED (code); localised output not run | `lpstat` is run with the user's locale (no setenv/LC_ALL=C before execvp) and parsed as English, so Polish/German desktops may list no printers. Locale-class sibling. | LabelPrinterLinux.cpp:22-67,131-166 |
| T23 | S2 | CONFIRMED (code) | Any DigiKey failure (429, network, token) is cached as "-" in `project->enrichment`; the queue skips cached keys, so it is never retried and is exported in the shortage CSV. | AppBomEnrichment.cpp:57-59,160-169 |

Cluster: T14, T15, T20 share a root cause (UI-held snapshot overwrites scanner commits). Fix once, e.g. defer `processDeviceSyncEvents` while edit/import is active, plus conflict check on commit.

## W S2 verification (orchestrator, main @ 0ce3761) - findings/W-windows-platform.md (S2 5, S3 16, S4 7; S3/S4 not yet verified)
DigiKey regression check for b6c9be6: reviewer found no Windows regression (CMake still selects WinHTTP DigiKeyTransport.cpp on Windows; winhttp link intact). Not compiled here.

| ID | Sev | Status | Finding | Evidence |
|---|---|---|---|---|
| T24 | S2 | CONFIRMED (code); Windows runtime not run | `environmentValue` uses `_dupenv_s` (ANSI), so LOCALAPPDATA/USERPROFILE with non-ACP characters resolve to the wrong path. Sibling of T13. | Environment.cpp:13-19 |
| T25 | S2 | CONFIRMED (code) | File/save/folder dialogs use the ANSI APIs (`GetOpenFileNameA`, `GetSaveFileNameA`, `SHBrowseForFolderA`), so non-ACP paths can't be chosen; MAX_PATH buffers. | Console.cpp:127,152,166 |
| T26 | S2 | CONFIRMED (code); both platforms | BLE provisioning (`.get()` waits, 25 s wait_for) runs on the UI thread; AGENTS.md requires workers. | InventatoryScanSetupPage.cpp:251, BleProvisioningService.cpp:163-218 |
| T27 | S2 | CONFIRMED (code); Winsock semantics from docs | Windows listener uses SO_REUSEADDR, no SO_EXCLUSIVEADDRUSE, so port-in-use detection fails across sessions/processes. Security-adjacent (availability). | HttpServerLifecycle.cpp:149-154 (no EXCLUSIVEADDR anywhere) |
| T28 | S2 | PLAUSIBLE (needs a Windows Terminal run) | Hide-to-tray relies on GetConsoleWindow(), which is a hidden pseudo-console window under Windows Terminal. | BackgroundController.cpp:101,364 |

Cluster: T13 + T24 + T25 = Windows ANSI-vs-UTF-8 path handling; one pass (wide APIs or UTF-8 manifest + /utf-8) fixes all three.

## K S2 verification (orchestrator, main @ 0ce3761) - findings/K-tests-build-ci.md (S2 3, S3 14, S4 7; S3/S4 not yet verified)
T8 sibling check: no other test target has the NDEBUG problem (inventatory_tests.cpp redefines assert as always-on; src/ has no assert/NDEBUG). Fix in progress via Gemini task g1.

| ID | Sev | Status | Finding | Evidence |
|---|---|---|---|---|
| T29 | S2 | CONFIRMED | No CI on pull_request or main: the only workflow is release.yml (tag push or workflow_dispatch needing an existing tag, publishes). | .github/workflows/release.yml:3-15; .github has only that file |
| T30 | S2 | CONFIRMED (skip logic); runner contents not checked | Locale regression tests skip silently (exit 0) when no pl/de/fr locale is installed, and the CI job installs none, so the locale class that broke unit search is untested where it matters. Test main() never calls setlocale. | inventatory_tests.cpp:581; release.yml has no locale step |
| T31 | S2 | CONFIRMED | App orchestration layer (src/app/*) is not in the test target (only AppBootstrap and AppSettings*); T11/T12/T14/T15/T20/T23 live there. 0 references to hasPendingPersistence/persistedStore_ in tests. | CMakeLists.txt:243-331 |

## J S1/S2 verification (orchestrator, main @ 5b9f169) - findings/J-ui.md (S1 1, S2 5, S3 15, S4 5; S3/S4 not yet verified; static review only)

| ID | Sev | Status | Finding | Evidence |
|---|---|---|---|---|
| T32 | S1 | CONFIRMED (code); FTXUI `reflect(Box&)` keeps a reference: library knowledge, source not available locally. Not reproduced under ASan. | `uiTargets_` is `reserve(512)`'d, then `push_back` + `reflect(uiTargets_.back().bounds)`. Frame with more than 512 targets reallocates the vector while earlier Reflect nodes hold dangling Box refs, which the layout pass then writes to (heap corruption). The Stock list creates one target per filtered item with no windowing (StockPageList.cpp:150,208), History one per commit, so ~490 parts or enough commits triggers it. | AppShellRender.cpp:73-74, AppInput.cpp:272-273, App.h:1077 |
| T33 | S2 | CONFIRMED (code) | No "Actions - Space" label/target anywhere in src, but docs say it is always visible and clickable: mouse-only users can't open the action sheet. | grep for the label in src/ui and src/app: none |
| T34 | S2 | CONFIRMED (code) | `InputMode::BomRestock` is absent from `showsPrompt`/`activePrompt`; buffer is prefilled with the shortage quantity and typed digits append invisibly (type "10" onto "5" -> 510 stock received). | AppBomProjectActions.cpp:379-380; BomRestock has no render references |
| T35 | S2 | CONFIRMED (code) | Mouse click sets `focusedTarget_` (index); Enter is routed to `activateFocusedTarget()` before page actions and nothing clears it on arrow/j/k, so after a click Enter re-activates the clicked row (Import accept, Stocktake count, Racks details, Projects restock). | AppInput.cpp:157-159,334,367 |
| T36 | S2 | PLAUSIBLE (row counts computed from code, no 100x30 capture) | Stock action sheet with a selection is ~35 rows plus frame vs 30 rows; overflow is clipped with no scrolling. | ActionRegistry.cpp:89-142,400-444 |
| T37 | S2 | CONFIRMED (code) | Projects shows "Ctrl+Z undoes it" after a build, but the Projects registry has no CtrlZ action (only History/Stock/Racks do). | ActionRegistry.cpp:75,133,183; Projects case at :295 |

Also verified: g1 (5b9f169) matches spec; T8 fixed.

## Fixes in progress
- T17: FIXED in agent worktree (uncommitted; .claude/worktrees/agent-a61a693631d01cb63, branch worktree-agent-a61a693631d01cb63): PhysicalValue.cpp parseRkmValue + tests. Harness red/green; full binary not built (sqlite.org blocked). Awaiting user approval to commit.
- T8 FIXED (5b9f169), T4 FIXED (6b28c7a), T21+T22 FIXED (6816b58), T1 FIXED (0ce3761 merge).
- Gemini batch 2 handed to user: g4 (T32), g5 (T34), g6 (T30 test side).

## New (from the T17 agent, orchestrator read BomMatchHelpers.cpp:90-128)
| T38 | S3 | PLAUSIBLE (sibling of T17) | BOM parser `parseNumberWithMultiplier` accepts an empty head and maps u/n/p/m to a multiplier even for resistance-like values, so unitless designations "M3"/"4u7" parse as resistances in BOM matching. Overlaps T18. | BomMatchHelpers.cpp:90-128, BomMatch.cpp:99-123 |
- T2: FIXED in agent worktree .claude/worktrees/agent-a03ae4a3285a6f3f8 (uncommitted): MdnsService publishes via Avahi D-Bus EntryGroup with the private interface index. Verified by the agent against a real avahi-daemon on 4 interfaces (service only on the private one; none on the other private veth), ASan/UBSan clean; full binary not built. Orchestrator read the diff: OK. CMake adds MdnsService.cpp to inventatory_tests (NOT WIN32). Residual: no re-registration after avahi restart/interface renumbering; start() blocks the UI thread ~0.8-1 s; COLLISION path untested. Awaiting user approval to commit.
- Running agents (not yet reviewed): T10 restore, T14/T15/T20 cluster, T5 CLOEXEC, T3 background lock.

## Landed on main (2026-10-05)
T1 0ce3761; T4 6b28c7a; T21+T22 6816b58; T8 5b9f169; T32 a19df4d; T34 910d964; T30(test side) 31001d8; J S3 empty-key + DigiKey empty secret b03a92f/50486cb; g9 135cdb8; g10 c2073df, g12 ci.yml a5b8e4b, g11 2e8b2b0 (Gemini, user-committed).
T17 790b93f; T2 01cc943; T3 fd70f6e; T5 d656abc; T10 f692eb3; T14+T15+T20 8b0ab19 (Claude agents, orchestrator-reviewed; full Debug+Release ctest-equivalent run with locale tests required, final tree before push).
Open: T6, T7, T9, T11, T12, T13, T16, T18/T38, T19, T23, T24/T25/T27/T28 (Windows), T26 (BLE on UI thread), T33, T35, T36, T37, S3/S4 lists.


## STATUS AT HANDOVER (2026-10-06)
On `main` (all built + tested Debug and Release on Linux with the real binaries before the push; Linux CI green):
- Earlier: T1, T4, T8, T21/T22, T30 (test side), T32, T34 (prompt visibility), g7/g8 (DigiKey empty secret, empty normalized key), g9, g10-g12 (HTTP negative tests, ci.yml, credential tests).
- Six Claude fixes 790b93f..8b0ab19: T17, T2, T3, T5, T10, T14+T15+T20.
- Wave A (23 commits, 8b0ab19..4d8e4de): T6 (EINTR), T7 (credential tri-state), T9, T11, T12 (back-off), replay-state fingerprint restart, shared JSON decoder, unit/launcher/lock fixes, systemctl/zenity/CUPS bounded helpers, update cleanup/cancel/preflight, atomic write durability, restore/backup durability and local-setting preservation, printer.conf write-on-change, keyring test skip + docs.
- 32f5260: Windows-only test fix (case-variant restore conflict). CI run 37430089428 for this commit was still running at handover: CHECK IT FIRST. Windows CI stops at the first failed assert, so more Windows-only test failures may appear one at a time.
NOT on main, pushed as branches for the next agent to integrate (see branch-notes/*.commits.txt):
- `claude/handover-domain-agent` (24 commits on top of 4d8e4de): T18/T38 BOM values, T19 placeholders, T23 failure cache, DF S3 items (reload guard, startup rewrite skip, rack index, validation scope, sqlite handle leak, folder-picker throw, findByCode, overflow, duplicate IDs, activity line limit, value parser, rack pointer), T16 streaming validation, CSV quote/category/encoding fixes, DigiKey credentials off UI thread / parameter lookup / optional fields + 401 refresh, shortage CSV atomic write, docs. The agent was stopped mid-task before its final report: it had committed everything; no integrated build/test result exists for the combined tree, and it was working on a categories commit.
- `claude/handover-ui-agent` (12 commits + 1 handover commit with 7 UNFINISHED, UNBUILT files on top of 4d8e4de): T35 Enter after click, T33 Actions header control, T36 action sheet scrolling, T37 Ctrl+Z on Projects, T34 follow-up replace default digit, nav-away unsaved prompt, wizard secret cleanup, R retry in scan setup, quick-label limit wording, import discard confirmation, Find-in-racks y-confirm, resize notice input block. The last commit (chore(ui): save unfinished UI work for handover) must be reviewed, completed or dropped.
Open / not done by anyone: see NEXT-AGENT-PROMPT.md.
