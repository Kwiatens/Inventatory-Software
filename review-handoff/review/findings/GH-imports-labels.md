# Slice G+H: Imports, DigiKey, Label printing

## Coverage

Read in full:
- G: `src/import/csv/{CsvReader,CsvFormat}.{h,cpp}`, `src/import/digikey/DigiKeyCsvImport.{h,cpp}`, `src/import/kicad/KicadBom.{h,cpp}`,
  all of `src/platform/digikey/*` (Api, ApiPrivate, Json, JsonParser, Product, ProductDetails, Transport (Windows), TransportLinux),
  `src/app/import/AppImportActions.cpp`, `src/app/bom/{AppBomActions,AppBomBuildActions,AppBomEnrichment,AppBomProjectActions}.cpp`.
- H: `src/label_printer/**` (core, layout, platform Linux/Platform/Rack, symbols/RackSymbols.cpp; Windows backend only skimmed for
  status/asymmetry), `src/app/labels/AppQuickLabelActions.cpp`, `src/app/racks/AppRackPrintingActions.cpp`,
  `processPrinterWork`/`stopPrinterWork` in `src/app/shell/AppRuntime.cpp`, `printLabelForItem`/`printSelectedLabel` in
  `src/ui/pages/printer/PrinterSetupPage.cpp`, `load/saveQuickLabels` in `src/app/settings/AppSettings.cpp`, `writeFileAtomically` in
  `src/core/storage/AtomicFile.cpp` (comparison), `CredentialStoreLinux.cpp:read`.
- Skipped: `RackSymbolsData.generated.cpp` (generated bitmap data), `LabelPrinterWindows.cpp` body (Windows-only, not the focus),
  `src/core/bom` (slice F). Not built or run. CUPS is not installed in this sandbox, so the `lpstat` output formats below come from
  knowledge of the CUPS source, not from a live run.
- Locale sweep for this slice: clean (no stod/strtod/printf/locale use; all ctype calls take unsigned char; floats go through
  `parseClassicDecimal`). Not re-reported: T7, T13, T18, T19, T11.
- Cleared after tracing: Linux libcurl transport (`DigiKeyTransportLinux.cpp`) has a connect timeout of 5 s and a total timeout of
  8 s, redirects off, TLS verification left at the libcurl defaults (verified), HTTPS-only prefix check, 4 MiB cap enforced in the
  write callback, headers never logged, `call_once` init, and the Retry-After parse is bounded. The `kMaximumDigiKeyResponseBytes`
  duplicate in the anonymous namespace compiles because the use is inside that namespace (a plain duplicate, see S4).

## Findings

### [S2] Import review commit overwrites scanner changes that arrived during the review (whole-store copy at begin, whole-store assign at commit)
- Location: src/app/import/AppImportActions.cpp:65-68, 120-124 (also 171-197); drain not gated in src/app/scanner/AppDeviceActions.cpp:127-222
- Category: persistence
- Failure scenario: User opens a DigiKey CSV with 40 rows and reviews them for several minutes. `beginCsvImport` snapshots
  `importStagedStore_ = store_`. While reviewing, the Scan R1 posts `inventory.adjust` / `inventory.receive` events;
  `processDeviceSyncEvents` has no import-stage guard, so it commits them to SQLite and updates `store_` / `persistedStore_`. On the last
  row `commitImportStage()` does `store_ = importStagedStore_` (the stale copy) and `saveState("import", ...)`. Quantity changes made
  by the scanner revert to their values from before the review. An item that the scanner created during the review (`resolveScanCode`
  `created`) is absent from the staged copy and is dropped from `store_`. The result is silent loss of device-committed inventory
  data. This is the import-stage sibling of T15 (edit) and T14 (scanner baseline).
- Evidence:
  ```
  importOriginalStore_ = store_;  importStagedStore_ = store_;           // :66-67
  bool App::commitImportStage() { ... store_ = importStagedStore_;       // :122
    if (!saveState("import", ...)) { importCommitPending_ = true; ...
  ```
- Confidence: high on the overwrite mechanism (code); medium on exactly how `saveState` diffs, because I did not trace the commit
  snapshot path.
- Proposed test: stage an import, apply a device quantity event to `store_`, commit the import, assert the event's quantity and any
  scanner-created item survive. This needs an App-level harness, or extract "apply staged candidates onto the current store" as a pure helper.
- Proposed fix (sketch): do not snapshot the whole store. Keep only the accepted candidate deltas and apply them to the then-current
  `store_` at commit (re-resolve conflicts by id/DigiKey part). Alternatively, defer `processDeviceSyncEvents` while `importStageActive_`
  (the events stay in the durable inbox).

### [S2] Linux: `probePrinter` reports a disabled/stopped CUPS queue as "available"
- Location: src/label_printer/platform/LabelPrinterLinux.cpp:168-182 (also 152-153)
- Category: correctness / linux-portability
- Failure scenario: CUPS prints a disabled queue as `printer Zebra disabled since Tue 05 Oct ... -` followed by `\treason unknown`
  (lpstat.c `printer %s disabled since %s -`). It uses the " is " form only for idle (`printer X is idle.  enabled since`);
  an active queue prints `printer X now printing X-12.  enabled since`. `probePrinter` searches for `" is "`, finds nothing, and returns
  `{true, "Printer queue is available"}`, so the Printer setup check says "ready" for exactly the case it exists to catch.
  Jobs are then accepted by `lp` and sit in a paused queue. The word "stopped" is also never printed by lpstat, so that branch is
  dead. `enumeratePrinters` handles the disabled case (it keys off "disabled"), so the two paths disagree. The Windows backend
  derives readiness from `PRINTER_STATUS_*` flags, so this is a Linux-only divergence.
- Evidence:
  ```
  const auto statusBegin = output.find(" is ");
  if (statusBegin == std::string::npos) return {true, "Printer queue is available"};
  ```
- Confidence: high for the logic given the CUPS strings above (medium only because there is no CUPS in the sandbox to run).
- Proposed test: extract the `lpstat -p` line parser into a free function and unit-test it with canned outputs: idle, now printing,
  disabled with a reason line, unknown queue, empty.
- Proposed fix (sketch): parse the first line the same way `enumeratePrinters` does (`printer NAME <rest>`); ready means the rest
  has neither "disabled" nor "not accepting". Share one parser between the two functions. Also consider `lpstat -a` for acceptance.

### [S2] Linux: `lpstat` output is parsed as English; localised CUPS output lists no printers
- Location: src/label_printer/platform/LabelPrinterLinux.cpp:22-67 (`runCommand`), 131-166
- Category: linux-portability (locale class)
- Failure scenario: `lpstat` is a CUPS client that localises its messages (the `cups_<lang>.po` catalogs: `printer %s is idle.`,
  `system default destination: %s`). The child inherits the user's `LANG` / `LC_*`. On a Polish, German or French desktop
  the lines no longer start with `printer ` or `system default destination: `, so `enumeratePrinters` returns an empty list (Printer
  setup shows no queues, so none can be chosen) and `probePrinter` never finds `" is "`. Combined with the previous finding it always
  reports "available". The app targets Polish users (the CSV importer carries Polish headers), so this is the normal environment for part of its audience.
- Evidence: `runCommand({"lpstat", "-p", "-d"}, output)` then `line.rfind("printer ", 0) == 0` and
  `line.rfind("system default destination: ", 0) == 0`. The child environment is not touched before `execvp`.
- Confidence: medium (the catalog existence and the exact translated strings are from memory; I did not run lpstat under pl_PL).
- Proposed test: with the extracted parser, assert the English fixtures. A manual check with `LANG=pl_PL.UTF-8 lpstat -p -d` on Ubuntu 24.04.
- Proposed fix (sketch): in the forked child call `setenv("LC_ALL", "C", 1)` (and unset `LANGUAGE`) before `execvp`, or use `lpstat -e`
  / `lpstat -p` through `env LC_ALL=C`. Better still, query via the CUPS API or IPP.

### [S2] BOM enrichment stores every transient failure as a permanent "-" result, which is persisted and exported
- Location: src/app/bom/AppBomEnrichment.cpp:160-169 (producer), :57-59 (skip rule), :89-92 (persist); consumers
  src/app/bom/AppBomBuildActions.cpp:232-236, src/ui/pages/bom/BomProjectPageRender.cpp:659
- Category: correctness / persistence
- Failure scenario: DigiKey is rate-limited (429), the network drops, the token POST fails, TLS fails, or the keyring was locked
  for one request. `fetchProductDetails` returns `nullopt` and the lambda returns `{key, "-", ...}`. `processBomEnrichment` writes
  `project->enrichment[key] = "-"`, sets `dirty_`, and `saveBomProjects()` persists it. `queueBomEnrichment` skips any key that is
  already in `project->enrichment` ("cached with the pinned project"), and re-import copies the map
  (`project.enrichment = existing->enrichment`, AppBomProjectActions.cpp:171). So after one bad run every affected line shows "-"
  forever and is never re-queried, even though credentials and network are fine later. A burst of 429s (the Linux transport sleeps at most
  2 s and replays once) poisons the whole BOM. The shortage CSV export then writes the sentinel "-" (tab-prefixed by `csvTextCell`) as
  the "Suggested DigiKey part".
- Evidence:
  ```
  if (const auto details = client->fetchProductDetails(keywords, &error)) { ... }
  return BomEnrichmentResult{key, string("-"), projectId, generation, requestSequence};   // any failure
  if (project->enrichment.count(key) != 0) continue;  // cached with the pinned project
  ```
- Confidence: high.
- Proposed test: factor the "result -> map entry" decision into a helper. Assert that a transport/HTTP failure leaves the key
  absent (retried next open) while an authoritative "no match" is cached, and that "-" is never exported.
- Proposed fix (sketch): distinguish "no match" from "error" (return the error class from the worker). Cache only no-match, and
  only error-free responses. Do not cache failures, or cache them in memory only for the session. Export an empty cell for
  absent/"-" suggestions.

### [S3] CSV scanner opens a quoted section in the middle of an unquoted field
- Location: src/import/csv/CsvReader.cpp:128-129 (and the in-quote leniency at 112-120)
- Category: correctness
- Failure scenario: `if (ch == '"') inQuotes = true;` fires wherever a quote appears, not only at field start. The comment says KiCad
  writes inch marks unescaped inside quoted fields, and that case is handled. But for an unquoted field such as `2.13" ePaper`
  (LibreOffice/Excel semicolon exports and hand-edited BOMs quote only when needed), the quote opens a quoted section that swallows the delimiters and newlines
  that follow, until another quote followed by a delimiter appears. Result: either "CSV has an unterminated quoted field" (the whole import is
  refused) or two rows silently merged into one cell/row. `sniffDelimiter` has the same toggle, so the delimiter can be mis-voted.
  Mid-field quotes are not RFC 4180 quote openings.
- Evidence:
  ```
  if (ch == '"') {            // outside quotes, any position in the field
    inQuotes = true;
  } else if (ch == delimiter) {
  ```
- Confidence: medium (depends on the producer not quoting such fields; KiCad itself quotes all fields).
- Proposed test: `parseCsv("Ref;Value\nR1;2.13\" ePaper\nR2;10k\n", ';', e)` should give 3 rows with the cell `2.13" ePaper`.
- Proposed fix (sketch): treat `"` as an opener only when the field so far is empty/whitespace; otherwise keep it literal. Apply the same
  rule in `sniffDelimiter`.

### [S3] DigiKey CSV `inferCategory` uses bare substrings, so unrelated parts are classified (and racked) wrongly
- Location: src/import/digikey/DigiKeyCsvImport.cpp:90-123 (consumed by core/racks/RackAllocation.cpp:32-86)
- Category: correctness
- Failure scenario: `text.find("uf")` / `"nf"` / `"led"` / `" ohm"` / `"res "` / `"ic "` match inside other words. Examples:
  `IC BUF NON-INVERT 5.5V SC70-5` contains "uf" and becomes "Capacitors" (the capacitor test runs before the IC test). `...176UFBGA`
  (UFBGA package) also hits "uf", as does `INFRARED` ("nf"). `SHIELDED`, `CONTROLLED`, `BUNDLED` hit "led" and become Indicators. `FERRITE BEAD 600 OHM`
  becomes Resistors. The category string is part of `classificationText()` used for rack auto-assignment, so `IC BUF ... SC70-5` is placed in the Capacitors
  rack because the text then contains "capacitors" and an SC70 package. The user reviews candidates, but the category is not the first thing they check.
- Evidence: `if (text.find("cap ") != npos || ... || text.find("uf") != npos || text.find("nf") != npos) return "Capacitors";`
- Confidence: high on the misclassification, medium on the rack consequence.
- Proposed test: `inferCategory` cases for BUF/UFBGA/shielded/ferrite bead. `parseDigiKeyCsvText` with those descriptions asserts the category is not Capacitors/Indicators/Resistors.
- Proposed fix (sketch): tokenize (alnum tokens) and match whole tokens or anchored prefixes. Put IC/MCU/FERRITE checks before the passives. Reuse the shared
  classifier (`core/parts`) instead of a private keyword list.

### [S3] `printer.conf` is rewritten on every `saveState`, and a failed load is overwritten with an empty config
- Location: src/app/persistence/AppPersistence.cpp:136, 278 (callers); src/label_printer/platform/LabelPrinterPlatform.cpp:137-172, 174-256
- Category: persistence (non-idempotent write class)
- Failure scenario: (1) `saveState()` always calls `printerService_.saveConfig(printerPath_)`, so every stock edit, scan or build writes a temp file,
  renames it over `printer.conf` and (on Windows) flushes it, even when nothing changed. With no printer configured it writes the content `""` and creates the
  file in every workspace. (2) `loadConfig` clears `configuredPrinter_` on any failure: a transient open failure (AV scan lock, permission blip, NFS
  hiccup) or the 4 KiB size cap. The result is ignored (`AppPersistence.cpp:103`). The next `saveState` then writes `""` over a file that was readable
  a moment ago, so the user's printer selection is lost silently. Because saveConfig returns true, the "printer settings" failure is not even reported.
- Evidence:
  ```
  configuredPrinter_.clear();  return false;        // every loadConfig failure branch
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");  // unconditional
  ```
- Confidence: high for the unconditional write; medium for the clobber (needs a transient read failure).
- Proposed test: `LabelPrinterService`: `saveConfig` twice leaves mtime/inode unchanged on the second call (or skip when unchanged); `loadConfig` on an unreadable
  or corrupt file followed by `saveConfig` must not replace the file.
- Proposed fix (sketch): track a `configDirty_` flag (set by `setConfiguredPrinter`, cleared on a successful load/save) and write only when dirty. Distinguish "missing" from
  "present but unreadable" in `loadConfig`, and refuse to overwrite the latter.

### [S3] Linux `printer.conf` write path: no fsync, no O_EXCL, duplicates `writeFileAtomically`
- Location: src/label_printer/platform/LabelPrinterPlatform.cpp:97-114, 238-249
- Category: persistence / linux-portability
- Failure scenario: Windows uses `CREATE_NEW` + `FlushFileBuffers` + `MoveFileExW(WRITE_THROUGH)`. The Linux branch opens an `ofstream` (trunc),
  `flush()`es (a libc buffer flush, not a disk sync), and `rename`s. A power loss or crash after the rename can leave an empty or truncated
  `printer.conf` on ext4/XFS delayed allocation, which `loadConfig` then rejects (see previous finding). The repo already has `writeFileAtomically` with POSIX
  fsync and exclusive create (`core/storage/AtomicFile.cpp`), used for settings and `quick_labels.conf`.
- Evidence: `ofstream output(path, ios::binary | ios::out | ios::trunc); ... output.flush(); output.close();` then `filesystem::rename(...)`.
- Confidence: medium.
- Proposed test: covered by the saveConfig tests above once it delegates to the shared helper.
- Proposed fix (sketch): replace `writeTemporaryConfig`/`temporaryConfigPath` and the rename block with `writeFileAtomically(path, contents, &error)`.

### [S3] CUPS child processes: no timeout, inherited descriptors, allocation after fork
- Location: src/label_printer/platform/LabelPrinterLinux.cpp:22-67, 184-226
- Category: linux-portability / concurrency
- Failure scenario: (a) `runCommand` and `sendRawJob` block in `read`/`waitpid` with no deadline. If `cupsd` or a remote `ServerName` is unreachable, `lpstat`/`lp`
  can stall; the single printer worker never completes, `printerWorkCompletion_` stays set, and every later print says "A printer job is already running"
  until restart (AppRuntime.cpp:101-112, 281). (b) `pipe()` is created without `O_CLOEXEC`, so any other `fork`/`exec` in the process during the window
  (avahi-publish-service, xdg-open, update helper; the same class as T5) inherits `inputPipe[1]`/`descriptors[]`. `lp` then never sees EOF on stdin while that child lives
  (avahi lives as long as the service), so the print hangs forever. Narrow window, severe effect. (c) The child allocates (`argv.reserve/push_back`) between `fork` and `exec` in a
  multithreaded process; this is not async-signal-safe (glibc mostly copes, but it is a latent deadlock).
- Evidence: `pipe(inputPipe)` ... `fork()` ... `while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}`; no `pipe2`, no alarm/poll timeout.
- Confidence: medium (a: plausible, depends on the CUPS client timeout; b: timing-dependent).
- Proposed test: backend seam test with a fake `lp` script that sleeps; assert `sendRawJob` fails within N seconds and kills the child.
- Proposed fix (sketch): `pipe2(O_CLOEXEC)`, build `argv` before `fork`, use `posix_spawnp` or at least `_exit`-only code in the child, `poll` with a deadline and `kill(SIGKILL)` + reap on expiry.

### [S3] DigiKey config (Secret Service D-Bus lookup) is loaded on the UI thread
- Location: src/app/bom/AppBomEnrichment.cpp:44, 132; src/app/import/AppImportActions.cpp:258, 299; src/app/common/AppActionSupport.h:239-246; platform/security/CredentialStoreLinux.cpp:41
- Category: linux-portability / concurrency (UI blocking)
- Failure scenario: `loadDigiKeyConfig()` reads `settings.conf` and calls `secret_password_lookup_sync`. `queueBomEnrichment` calls it every time a BOM project is
  opened or imported, `processBomEnrichment` calls it when creating the client, and `beginImportSync` calls it twice (`createDigiKeyApi` then `loadDigiKeyConfig`).
  With a locked login keyring (the lookup triggers an unlock prompt) or a session without a reachable D-Bus, the sync call blocks the render loop until
  the user answers or the call times out. Windows Credential Manager does not have this failure mode. AGENTS.md forbids slow work on the UI thread.
- Evidence: `if (!loadDigiKeyConfig().valid()) { return; }` in `queueBomEnrichment` (UI thread).
- Confidence: medium.
- Proposed test: none practical at unit level; verify manually with a locked keyring.
- Proposed fix (sketch): resolve the config once inside the worker (or cache a validated `DigiKeyConfig` with a "credentials present" flag) and pass it in; never
  call `CredentialStore` from the render path. Remove the duplicate load in `beginImportSync`.

### [S3] `readParameterValue` returns the first loosely matching parameter and the caller then discards it, so the real "Package / Case" is never examined
- Location: src/platform/digikey/DigiKeyProduct.cpp:117-155; caller DigiKeyProductDetails.cpp:101-117
- Category: correctness
- Failure scenario: matching is symmetric substring: `normalizedLabel.find(needle) || needle.find(normalizedLabel)`. DigiKey's packaging parameter is labelled
  "Package" (the code renames it to "Packaging" elsewhere), which is a substring of the needle "packagecase", so when it appears before "Package / Case" in
  `Parameters` it is returned first. `extractComponentPackage` then drops it via `looksLikePackagingType` ("Tape & Reel (TR)") and returns `nullopt`, never trying the
  later "Package / Case" entry, so the package falls back to description regex guessing. Also, a label that normalises to empty (for example "-") makes
  `needle.find("")` true and matches every needle. Parameter order from DigiKey is not guaranteed.
- Evidence:
  ```
  if (normalizedLabel == normalizedNeedle || normalizedLabel.find(normalizedNeedle) != npos ||
      normalizedNeedle.find(normalizedLabel) != npos) { ... return trimmed; }
  ```
- Confidence: medium (the order of DigiKey's parameter array was not verified).
- Proposed test: feed `parseProductDetails` a product with `Parameters` ordered [Package=Tape & Reel (TR), Package / Case=0603 (1608 Metric)] and expect `packageName == "0603 (1608 Metric)"`. A second case with an empty-normalising label.
- Proposed fix (sketch): pass a predicate into the search (skip packaging values and keep scanning), require `normalizedLabel` non-empty, and use exact match first, then a one-directional prefix.

### [S3] Shortage CSV export ignores write failures and is not atomic
- Location: src/app/bom/AppBomBuildActions.cpp:217-239
- Category: persistence / correctness
- Failure scenario: `ofstream output(target, ios::binary)` is only checked for open. Rows are streamed and the code then reports "Wrote N shortages". On a full disk,
  a removed USB stick or a quota error the file is truncated and the success message is still shown. Overwriting an existing export also destroys the previous one before the new one is complete.
- Evidence: `output << ... "\r\n"; ++rows; } setMessage("Wrote " + to_string(rows) + " shortages to " ...); return true;`
- Confidence: high.
- Proposed test: build the CSV text in a pure function (also testable for the "-" sentinel and formula escaping) and write it with `writeFileAtomically`, asserting the failure path.
- Proposed fix (sketch): assemble into an `ostringstream`, call `writeFileAtomically`, report its error.

### [S3] Non-UTF-8 CSV bytes are imported verbatim (Excel "CSV" in Windows-1250/1252, UTF-16 "Unicode text")
- Location: src/app/import/AppImportActions.cpp:38-50; src/import/csv/CsvReader.cpp (no encoding step); header matching at CsvReader.cpp:179-189
- Category: correctness (encodings)
- Failure scenario: Polish Excel's "Zapisz jako CSV" writes Windows-1250 (no BOM). A DigiKey order with `10µF` (0xB5) or a Polish manufacturer/description is read as-is: header
  matching still works by accident (`normalizeHeader` drops every non-ASCII byte, so "Ilość" becomes "ilo"), the rows are accepted, and invalid UTF-8
  goes into `partName`/`manufacturer`, then into SQLite, snapshots, JSON and ZPL (same effect class as T1). A UTF-16LE file (BOM FF FE) parses as NUL-interleaved
  garbage and fails with the misleading "CSV is missing required DigiKey order columns". CR-only line endings yield a single row.
- Evidence: `const auto text = buffer.str(); detectCsvFormat(text)` with only `stripByteOrderMark` (UTF-8 BOM) applied.
- Confidence: medium (depends on how the user exports; `normalizeHeader` behaviour verified by reading).
- Proposed test: feed `parseDigiKeyCsvText` a Windows-1250 buffer with 0xB5 and a UTF-16LE buffer; expect either a transcoded UTF-8 candidate or a clear "unsupported encoding" error, never invalid UTF-8 in `partName`.
- Proposed fix (sketch): after BOM handling validate UTF-8; on failure detect UTF-16 BOM and transcode, else offer/try cp1250/cp1252 transcoding or reject with a specific message. Treat lone CR as a line break.

### [S3] DigiKey details: optional numeric field rejects the whole product; fixed token lifetime without 401 recovery
- Location: src/platform/digikey/DigiKeyApi.cpp:258-264, 115, 120-122
- Category: correctness (JSON robustness)
- Failure scenario: (a) `ManufacturerLeadWeeks` or `UnitPrice` that is not a pure unsigned decimal / classic decimal (text such as "12 Weeks", an exponent form
  outside `parseClassicDecimal`'s grammar) makes `parseDetails` return `nullopt`, so the entire result (description, parameters, datasheet) is
  discarded for a display-only field and the caller sees a failed sync (retry later hits the same payload). (b) `tokenExpiresAt_` is hard-coded to now+540 s
  and `expires_in` is ignored; a 401 is not treated as "refresh token and retry once", so a token revoked or shortened server side produces up to 9 minutes of failures (each of which BOM enrichment then caches, see S2).
- Evidence: `if ((!quantityAvailable.empty() && !isUnsignedDecimal(...)) || (!manufacturerLeadWeeks.empty() && !isUnsignedDecimal(...)) ...) return optional{};`
- Confidence: low (actual DigiKey payload formats were not checked; the code path is verified).
- Proposed test: `parseProductDetails`+validation with `"ManufacturerLeadWeeks":"12 Weeks"` expects details with the field cleared, not a rejection.
- Proposed fix (sketch): blank the invalid optional field instead of failing; honour `expires_in` (minus a margin); on 401 drop the token and retry once.

## Test gaps
- `parseProductDetails`, `resolveSearchResult`, `readParameterValue`, `extractComponentPackage`, `extractInductanceFromText` (DigiKey side): no tests (only `validateDigiKeyJsonPayload`, `isFiniteDecimal` and header-validation tests exist) -> feed canned v4 ProductDetails/keyword JSON (parameters order, ExactMatches vs Products, missing ProductVariations, duplicate keys, 4 KiB string).
- Linux transport helpers (`encodeComponent`, `escapeJsonString`, `widen`/`narrow` round trip and invalid UTF-8, Retry-After parsing, oversized body via a local HTTPS test server or a seam over `requestHttpOnce`): none -> add a seam and tests for HTTP 429/5xx retry (GET only), non-GET never retried, oversize -> error.
- CUPS backend (`enumeratePrinters`/`probePrinter` parsing, `sendRawJob` failure/early-close/EPIPE, timeout): not unit-testable today -> extract parsers; use a fake `lp`/`lpstat` on PATH.
- `LabelPrinterService::loadConfig/saveConfig`: no test (only quick-label load/save are tested) -> round trip, quoted names with spaces, oversize file, corrupt file, no-op rewrite, failed-load-then-save.
- CSV: mid-field quote, CR-only and mixed line endings, quoted CRLF inside a cell, semicolon file with inch marks, non-UTF-8 and UTF-16 input, `1,000` grouped quantities in DigiKey rows.
- `inferCategory` (private) and the DigiKey CSV categories end to end: BUF/UFBGA/shielded/ferrite bead false positives.
- Import workflow: commit while a device event is applied (App-level), `beginCsvImport` over-limit/unreadable messages, sync cancel/retry bookkeeping (`importSyncFailedItemIds_` counts).
- BOM enrichment: failure vs no-match caching; `bomEnrichmentScopeMatches` is presumably covered, the "-" persistence is not.
- `exportBomShortages`: CSV content, "-" sentinel, write failure.
- Quick-label HTTP path: `printDeviceQuickLabel` idempotency, stale revision, queue-full, completion after workspace change (cache entry left "pending" when the result is dropped at AppRuntime.cpp:121).

## Quality (S4)

### [S4] Duplicated constants and curl setup differ from the rest of the Linux code
- Location: src/platform/digikey/DigiKeyTransportLinux.cpp:24, 154-167, 177-183, 28-30; src/platform/system/UpdateService.cpp:441-458, 780-851
- `kMaximumDigiKeyResponseBytes` is defined in the header and again in the anonymous namespace (compiles only because of lookup order). `UpdateService` sets `CURLOPT_NOSIGNAL` and
  `CURLOPT_PROTOCOLS_STR "https"`; the DigiKey handle sets neither. Failures return a generic "Unable to complete the DigiKey HTTPS request" with no `CURLOPT_ERRORBUFFER` / `curl_easy_strerror`, so TLS vs DNS vs timeout is
  indistinguishable in the UI. Three independent `curl_global_init` call sites (two function-local statics plus `call_once`) are not mutually serialised; harmless on libcurl >= 7.84 but a shared `ensureCurlInitialized()` would be cleaner.

### [S4] `std::regex` objects are constructed on every call
- Location: src/platform/digikey/DigiKeyProductDetails.cpp:47-48 (19 patterns compiled per `extractComponentPackageFromText` call), :89 (`extractInductanceFromText`); src/label_printer/core/LabelPrinterDetails.cpp:142 (`hasUnit`, compiled per tile candidate per parameter), src/label_printer/core/LabelPrinterText.cpp:65
- Compile once (`static const`), or replace the package patterns with one alternation / hand-written token scan. `extractInductanceFromText`, `looksLikeInductanceValue`, `canonicalInductanceUnit` and `looksLikeFrequencyValue` are implemented twice (digikey_detail and label_printer_detail).

### [S4] Import helpers contain redundant or misleading logic
- DigiKeyCsvImport.cpp:166-170: `(A || B) && B` is just `B` (`looksLikeManufacturerPart`). `parsePositiveInt` accepts 0, name is misleading; `parsePositiveInt` (DigiKey) and `parseCount` (KiCad) are the same function.
- `kMaximumImportBytes` is defined three times (DigiKeyCsvImport.cpp:23, KicadBom.cpp:21, AppActionSupport.h:29), and the 25 MiB check runs in `beginCsvImport`, in `parse*Text` and in `load*File`. `beginCsvImport` (AppImportActions.cpp:38-50) re-implements `loadDigiKeyCsvFile` without the `input.bad()` check, and reports "exceeds the 25 MiB safety limit" when `file_size` itself failed.
- `productSearchUrl` hard-codes `https://www.digikey.com/en/...` regardless of the configured site/language.
- `details.vendorMetadata.categoryId = details.categoryName` (DigiKeyProductDetails.cpp:250): a name stored in an id field.

### [S4] `fitFont0Text` ellipsis loop over-trims after a multi-byte character
- Location: src/label_printer/layout/LabelPrinterTextLayout.cpp:334-340
- Popping an ASCII last character while the previous character is multi-byte also pops that multi-byte character (the do/while strips continuation bytes of the character *before* the one removed, then the lead byte), so "10kΩa" loses two characters per step and the ellipsized text is shorter than necessary. Cosmetic; simplify to "remove one code point per iteration".

### [S4] Substring heuristics with very short needles in label context headers
- Location: src/label_printer/core/LabelPrinterContext.cpp:302-312 ("nor", "can", "spi", "phy", "usb", "mux"), :256-258 ("osc"), :393 ("fet"); `itemTextContains` scans manufacturer, location and notes
  (LabelPrinterText.cpp:103-132)
- "nor" matches "Nordic"/"Normal", "can" matches "scanner", "fet" matches "safety". The existing `itemTextHasToken` is the right tool for short words. Wrong header text on a label is cosmetic but hard to notice.

### [S4] BOM/CSV code smells
- `AppBomProjectActions.cpp:142-146`: `beginBomProject` assigns `bomFile_` before checking `ok`, so a failed import leaves `bomFile_` describing the failed file while `activeBomProjectId_` still names the previous project (only reachable from the project list; harmless today but fragile). Parse into a local and move on success.
- `processImportSync`/`beginImportSync` bookkeeping (`importSyncCompleted_`, failed counts added to both vectors and counters) is hard to follow; `importSyncFailedCount_` is reset in `beginImportSync(true)` so the final "N sync failed" message only counts the last retry round.
- `AppRackPrintingActions.cpp:1-3` carries two header comments; `sendRawJob` discards `lp` stderr (`/dev/null`) so the user only sees "CUPS did not accept the raw label job" with no reason (capture stderr like `runCommand` does).
