# Slice C+E: Scan R1 protocol (security boundary) and backup/restore

## Coverage
Read in full: docs/scanner-transport-security.md (first 60 lines), docs/backup-restore.md, src/core/scanner/{InventatoryScanProtocol.cpp, ...Security.cpp, ...JsonParser.cpp, ...Json.cpp, ...Config.cpp, DeviceSyncStore.cpp}, src/platform/scanner/{HttpServer.cpp, HttpServerConnection.cpp, HttpServerProtocol.cpp}, src/app/scanner/AppDeviceActions.cpp, relevant parts of AppScanEnrichment.cpp / AppQuickLabelActions.cpp / AppRuntime.cpp / InventatoryScanSetupPage.cpp, src/core/transfer/* (Backup, Restore, Recovery, Manifest, Validation, Ops, Private.h), src/app/persistence/AppBackupRestore.cpp, src/ui/pages/settings/SettingsPageSave.cpp, src/core/storage/AtomicFile.cpp and createSqliteSnapshot, test grep of tests/inventatory_tests.cpp.
Skipped: HttpServerLifecycle.cpp/MdnsService/BLE (not in the slice or already known), HttpServer.h, the remainder of docs/scanner-transport-security.md (response-size section), AppScanEnrichment DigiKey logic.
Checked and found sound (not findings): HMAC covers method + path + device + counter + exact body bytes, and method/path are the server's own parsed values; header and body deviceId are cross-checked; the compare is constant-time; request/response keys are directional; the counter is persisted before the response is sent (after the callback, before the ack); the callback is serialised with a single in-flight reservation; callback exceptions are caught; duplicate headers, transfer-encoding and body-length mismatches are rejected; the manifest has an allowlist, no path separators, SHA-256 and size checks, and a bundle with unknown files or symlinks is rejected; the pre-restore backup is created and validated (published only after validation) before the destination is touched; restore is journaled with a rollback path; the restore token is rotated and the replay state removed.

## Findings

### [S1] Restore destroys every file in the data directory that is not on the backup allowlist
- Location: src/core/transfer/InventoryRestore.cpp:155 and :173; src/core/transfer/InventoryTransferRecovery.cpp:157 (oldData removeAll); src/ui/pages/settings/SettingsPageSave.cpp:27-34 (no emptiness/ownership check on the chosen data directory)
- Category: persistence
- Failure scenario: The data directory is any user-chosen folder; `saveSettingsDraft` only calls `create_directories`. A user who points it at an existing folder (for example `~/Documents/Inventatory` containing CSV exports, datasheets or label PDFs, or `~/Documents` itself) and then runs Restore backup gets the whole directory renamed to `<dir>.restore-old-data-*`, replaced by a staging directory holding only the five allowlisted files, and the old directory is then `remove_all`ed in `cleanupCommittedRestore`. The pre-restore backup only contains the allowlisted files (`createInventatoryBackup` copies only inventory.db, activity.tsv, printer.conf, quick_labels.conf and a sanitized settings.conf). Everything else is permanently lost, although the docs say the restore only replaces inventory data.
- Evidence: `ops.rename(destinationDirectory, journal.oldData, primaryError)` then `ops.rename(staging, destinationDirectory, primaryError)`; cleanup: `if (oldDataExists && !ops.removeAll(journal.oldData, stepError))`
- Confidence: medium (the code path is certain; it depends on users choosing a non-dedicated folder, which nothing prevents)
- Proposed test: create `restoreTarget/notes.txt` and `restoreTarget/sub/x.bin`, restore, assert both survive (or that restore refuses with a clear error before activation).
- Proposed fix (sketch): before activation, enumerate the destination. Either move non-allowlisted entries into the staging directory, or refuse and tell the user the folder contains unmanaged files. Alternatively replace only the allowlisted files individually. At minimum, include unmanaged entries in the pre-restore backup, or delay `remove_all` of old data until the user confirms.

### [S2] HTTP worker thread reads UI-owned `LabelPrinterService` without synchronization
- Location: src/app/labels/AppQuickLabelActions.cpp:133 (also :46 is UI-thread, fine); written at src/ui/pages/settings/SettingsPageSave.cpp:294, src/app.cpp:134, AppPersistence.cpp:103
- Category: concurrency
- Failure scenario: `handleDeviceSync` runs on an HTTP worker thread and, for a `quickLabelPrint` request, calls `printDeviceQuickLabel`, which calls `printerService_.configuredPrinter()` (returns `const string&` to the member) with no lock. Meanwhile the UI thread saves settings with the bridge still running (the data-folder or port-unchanged path in `saveSettingsDraft`) and executes `configuredPrinter_ = trim(printerName)`, a `std::string` assignment. This is a data race on a std::string (torn read, use-after-free of the old heap buffer). AppRuntime.cpp:236 states "No worker ever touches the UI-owned service", which this call violates. AGENTS.md requires worker/UI state to go through the existing mutex or queue discipline.
- Evidence: `string printerName = printerService_.configuredPrinter();` in `App::printDeviceQuickLabel`; `configuredPrinter_ = trim(printerName);` in LabelPrinterPlatform.cpp:259.
- Confidence: high for the race, low for the observed probability
- Proposed test: not unit-testable deterministically; run a TSAN build with a quick-label request concurrent with a settings save.
- Proposed fix (sketch): keep a copy of the configured printer name under `quickLabelMutex_` (or a new small mutex) updated whenever the UI changes it, and read that copy in `printDeviceQuickLabel`; or resolve the printer name on the UI thread when the queued PrinterWork is processed (the work already carries `printerName` only for the worker thread).

### [S2] Persistent commit failure of a device event makes the UI thread spin and grows the DigiKey enrichment queue
- Location: src/app/scanner/AppDeviceActions.cpp:127-217 (specifically :137, :196, :214-217)
- Category: correctness / perf
- Failure scenario: `processDeviceSyncEvents` reloads the oldest `received` event, sets `deviceSyncEventsHint_` to true ("keep draining") at line 137, copies the whole inventory (`auto candidate = store_`), builds a result and calls `completeDeviceSyncEvent`. If the commit fails persistently (disk full, read-only data folder, DB locked or a baseline conflict), the function sets a message and returns, but the hint is still true. Every render-loop iteration repeats the SQLite read, the full store copy and the commit attempt, so the UI thread is busy and the status message is re-set continuously. Each attempt for an `inventory.receive` also pushes `(item->id, code)` onto `scanDigiKeyEnrichmentQueue_` before the commit (line 196), so the queue grows without bound while the failure persists, and the entries refer to items that were never committed. Head-of-line blocking: `loadPendingDeviceSyncEvents(..., 1)` always returns the same failing event, so later events are never processed.
- Evidence: `deviceSyncEventsHint_.store(true);  // keep draining until the inbox is empty` ... `if (!completeDeviceSyncEvent(...)) { setMessage("... could not be committed", 4); return; }`
- Confidence: medium (the loop frequency of processDeviceSyncEvents in AppShell.cpp:210 was not measured, but it is called each tick)
- Proposed test: with a read-only inventory path or a hook that fails commit, call `processDeviceSyncEvents` N times and assert the enrichment queue stays bounded and that a retry back-off applies.
- Proposed fix (sketch): on commit failure, clear the hint and apply a back-off (for example retry no sooner than 5 s); only enqueue enrichment after a successful commit; consider marking a repeatedly failing event as `failed` so it does not block the inbox.

### [S3] Replay-state fingerprint mismatch at startup locks the scanner out permanently (until re-pair)
- Location: src/platform/scanner/HttpServerProtocol.cpp:206; src/platform/scanner/HttpServer.cpp:83-84; sources of removal e.g. src/ui/pages/scanner/InventatoryScanSetupPage.cpp:71,131,270 and src/app/persistence/AppBackupRestore.cpp:182
- Category: correctness
- Failure scenario: `loadReplayState` returns false when the stored fingerprint differs from the current token's fingerprint, and `setDeviceCredentials` then sets `replayStateValid_ = false` so every request is answered `409 Replayed or unavailable request counter`. A different fingerprint means a different key, so a counter reset to 0 is cryptographically safe, but the code treats it the same as a corrupt file. Scenario: the user rotates or clears the pairing, the token is written to the credential store, and the process crashes (or the `filesystem::remove(...)` whose error code is ignored fails) before the old `inventatory-scan-replay.state` is removed. On restart the server starts with the new token and the old fingerprint, is invalid, and the R1 receives 409 forever with no UI hint. The in-process rotation path does not have this problem (`pairingChanged` branch), only a cold start does.
- Evidence: `return !input.bad() && hasFingerprint && hasCounter && storedFingerprint == fingerprint;`
- Confidence: high (logic), medium (reachability: needs a crash or a failed unlink in a narrow window)
- Proposed test: write a state file with fingerprint A, call `setDeviceCredentials(device, tokenB, path)`, start, send counter 1 signed with tokenB, expect 200 and a rewritten state file.
- Proposed fix (sketch): distinguish "well-formed but different fingerprint" (reset to counter 0, valid) from "malformed/unreadable" (fail closed). Also check the result of the state removal in the rotate/clear/restore paths.

### [S3] A missing replay-state file is treated as a fresh sequence (fail-open) even for an already-paired workspace
- Location: src/platform/scanner/HttpServerProtocol.cpp:163-166, 209-214; src/platform/scanner/HttpServer.cpp:83
- Category: security
- Failure scenario: `loadReplayState` returns true with counter 0 when the file does not exist. Deleting or losing the file (cleanup tool, restoring the folder from a file-level backup that omitted it, a failed atomic rename after power loss, see the fsync finding) re-opens every counter above 0 for the unchanged token. A LAN attacker who recorded earlier authenticated sync requests (the protocol is integrity-only, so request bodies are visible and replayable) can replay them; the events are re-inserted into the inbox if their rows have since been pruned (`pruneAcknowledgedResults` keeps only 256 acknowledged results per device) and are then re-applied to stock. This is a narrow precondition but is the documented replay guarantee ("including after restart").
- Evidence: `if (!exists) return true;` after `counter = 0;`
- Confidence: medium
- Proposed test: pair, accept counter 50, delete the state file, restart the server, replay counter 50, expect rejection.
- Proposed fix (sketch): write the state file (counter 0, fingerprint) at pairing time so that a missing file for a paired workspace means "invalid" and requires explicit re-pair; or store the high-water mark also in the settings/SQLite data.

### [S3] Restore journal, manifest and published backup bundle are written without fsync (no durability before "atomic" rename)
- Location: src/core/transfer/InventoryTransferOps.cpp:381-403 (`atomicWriteText`, `ofstream` + flush/close, then rename), :299-316 (`replace` on Linux is plain `rename`), :266-275 (`copy_file` for staged files), src/core/transfer/InventoryTransferManifest.cpp:280-295 (`writeManifest`), src/core/transfer/InventoryBackup.cpp:195 (rename of staging to the destination bundle); src/core/storage/AtomicFile.cpp (fsyncs the temp file but never the parent directory)
- Category: persistence / linux-portability
- Failure scenario: On Linux there is no `fsync` of the journal, the staged files, the manifest, or any directory, while the journal states and directory renames are the crash-recovery protocol. After a power loss mid-restore the directory renames can be persisted while the journal file (or the journal's latest state) is zero-length or stale (ext4 delayed allocation, XFS, btrfs); recovery then either finds a malformed journal (`Restore journal is malformed`, startup stays in recovery-required) or acts on an older state than the on-disk layout. Likewise a freshly "validated" backup bundle published by rename can contain zero-length files after a crash, which the user believes is a verified backup. (The SQLite snapshot itself is durable because SQLite fsyncs on commit.) The Windows branch uses MOVEFILE_WRITE_THROUGH for the rename only, not for file contents.
- Evidence: `output.write(...); output.flush(); output.close();` then `ops.replace(temporary, target, error)`.
- Confidence: high that no fsync exists; medium on practical frequency
- Proposed test: not unit-testable; assert via an injected hook that `atomicWriteText` calls the shared fsync path.
- Proposed fix (sketch): reuse `writeFileAtomically` (already fsyncs the file) for the journal and manifest, add an fsync-directory helper (open the parent with O_DIRECTORY|O_RDONLY and fsync) used after every journal rename and after publishing a bundle, and fsync staged copies before the validated rename.

### [S3] Symlinked data directory makes backup and restore impossible, with a misleading error
- Location: src/core/transfer/InventoryRestore.cpp:85-89, src/core/transfer/InventoryBackup.cpp:50-53
- Category: linux-portability
- Failure scenario: A Linux user whose data folder is a symlink (stow/dotfile managers, data moved to another disk and linked) gets "Restore destination is not a directory" (the branch is hit because `isLinkedOrReparseArtifact` is evaluated in the same condition as `is_directory`) and "must not be a link" for backup. Backup is refused even though only reading is involved. The restore error text blames the type, not the link, so the user cannot tell why.
- Evidence: `(isLinkedOrReparseArtifact(destinationDirectory, filesystemError) || !filesystem::is_directory(...))` -> `error = "Restore destination is not a directory";`
- Confidence: high
- Proposed test: symlinked data dir -> `createInventatoryBackup` succeeds (read-only operation); restore gives an explicit "link" message.
- Proposed fix (sketch): permit a link for backup (resolve with `canonical`); for restore, resolve the link target and operate on the canonical directory (renames need the real parent), or split the error messages.

### [S3] Restore applies `background_service_enabled`, port and consent flags from the bundle without reconciling system state
- Location: src/core/transfer/InventoryRestore.cpp:189-198; src/app/persistence/AppBackupRestore.cpp:139-153; AppShell.cpp:124-126 (reconciliation happens at next start)
- Category: correctness
- Failure scenario: The bundle's sanitized `settings.conf` keeps `background_service_enabled`, `background_consent_asked` and `device_service_port`. Restoring a bundle made on another machine/OS (or earlier) writes these into the active settings. On the next launch `AppShell` calls `setBackgroundStartupEnabled(true)` and registers the autostart (systemd unit/.desktop) without the user ever answering the consent prompt on this machine; in the current session the settings file claims a state the OS does not have. A restore from a bundle with a different `device_service_port` also changes the bridge port silently, so the R1 (provisioned with the old port or relying on mDNS) may not reach it.
- Evidence: `restoredSettings.dataDirectory = destinationDirectory;` (only `dataDirectory` is overridden before `saveRestoreSettings`)
- Confidence: medium
- Proposed test: restore a bundle with `background_service_enabled=true` into a profile with it false; assert the active settings keep the local values for machine-specific keys.
- Proposed fix (sketch): when restoring, keep the machine-local keys from the existing settings (background, consent, port, printer queue) and take only workspace-level keys from the bundle; or strip them at backup time.

### [S3] Scan setup sequence blocks the UI thread behind a worker that holds the credential lock while waiting for the callback mutex and during `sendAll`
- Location: src/platform/scanner/HttpServerConnection.cpp:64-117, 138-147; src/platform/scanner/HttpServer.cpp:60
- Category: concurrency
- Failure scenario: A worker takes `credentialOperationMutex_` (line 64) and blocks on `callbackSerialMutex_` (line 116) while another worker still holds it. That other worker's `callbackLock` lives to the end of the `if` block, past `advanceReplayCounter`, response serialisation and `sendAll` (up to the socket send timeout for a slow client), and after `advanceReplayCounter` the single-reservation guard has been cleared, so a new request can pass `reserveReplayCounter` and wait on the callback mutex while holding the credential lock. `setDeviceCredentials` (UI thread, rotation, clear pairing, restart service) does `lock_guard<mutex> operationLock(credentialOperationMutex_)` unconditionally and therefore blocks the terminal UI for as long as the slow client holds the send. The comment at HttpServer.cpp:54-59 says rotation must not wait for callbacks, but it can.
- Evidence: `unique_lock<mutex> callbackLock(callbackSerialMutex_); credentialLock.unlock();` (callbackLock stays alive through `sendAll`).
- Confidence: medium
- Proposed test: slow-reading client plus concurrent second request, then call `setDeviceCredentials` and assert it returns within a bound.
- Proposed fix (sketch): scope `callbackLock` to the callback only (`{ lock; callback(...); }`), or make `setDeviceCredentials` use try_lock/epoch only.

### [S3] Divergent JSON string decoders: `\u` escapes and `\b \f` corrupt text, and `resultAcks` decode differently from event ids
- Location: src/core/scanner/InventatoryScanProtocolJson.cpp:31-48 and :216-228
- Category: correctness
- Failure scenario: `jsonString` turns `é` into a single raw byte 0xE9 (invalid UTF-8), `\u0000` into an embedded NUL, and any `\uXXXX` above 0xFF into the literal text `uXXXX` (the `\` is dropped, the hex digits stay); `\b` and `\f` become the letters `b` and `f`. `jsonStringArray` (used for `resultAcks`) pushes the character after the backslash (so `\n` becomes `n`, `é` becomes `u`). An event id or code containing an escape therefore decodes to different strings in `events[].eventId` and `resultAcks[]`, so an ack would never match the stored event id, and NULs are truncated by `bind_text(..., c_str())`. Current firmware likely emits ASCII ids, so this is latent; a non-ASCII barcode code (for example a part number with a Greek mu) would be stored corrupted.
- Evidence: `if (codepoint <= 0xFFU) { value.push_back(static_cast<char>(codepoint)); ...` and `value.push_back(ch);` for the other escapes.
- Confidence: high (behaviour), low (practical impact today)
- Proposed test: `parseDeviceSyncRequestJson` with `"code":"µF"` and `€` and `\\n` ids; assert UTF-8 decoding and equal decoding for events and resultAcks; reject `\u0000`.
- Proposed fix (sketch): one shared unescape routine producing UTF-8 (with surrogate pairs), rejecting NUL; use it in both decoders.

### [S3] Authenticated requests that are rejected do not consume the counter, and a failed callback releases it, so the exact request stays replayable
- Location: src/platform/scanner/HttpServerConnection.cpp:100-131
- Category: security
- Failure scenario: Parse failures (400/426) and callback failures (503) return before or release the reservation, so the captured authenticated request with counter N stays valid until a later counter is accepted. An active LAN attacker who blocks the R1's retry (or replays during an outage) can deliver request N later; events are idempotent (INSERT OR IGNORE) so the damage is limited to out-of-order delivery of acks/lookups and quick-label print requests (the print is cached by requestId, so no duplicate print). Documented behaviour says "refuses lower or equal counters", which holds only after success. Low impact, noted for the threat-model text.
- Evidence: `releaseReplayCounter(*counter, credentialEpoch); ... return reject(503, error);`
- Confidence: medium
- Proposed test: callback fails with 503 for counter N, later accept N+1, replay N -> 409 (exists via retry test at 4014 only for the happy retry; add the stale-after-failure case).
- Proposed fix (sketch): optional; document it, or persist "highest seen" for authenticated attempts too.

## Test gaps
Scan R1 (tests/inventatory_tests.cpp currently covers: body tamper -> 401, equal-counter replay -> 409, rotation with old token -> 401, restart persistence, corrupt/malformed-fingerprint state, concurrent duplicate, marker write failure, duplicate Content-Length, oversize response, slow clients, DB-level wrong-device completion, duplicate event ids, deep JSON nesting).
Missing from the required list:
- Wrong method (GET or PUT to /api/v1/device/sync) and wrong path (/api/v1/device/other, trailing slash, query string, case variants) with a MAC computed for the other method/path -> expect 404 or 401; helper `signedSyncRequest` hard-codes POST and the path.
- Wrong device: header device != paired device (400 branch at HttpServerConnection.cpp:97), header device != body deviceId (line 102), and a MAC computed over a different deviceId.
- Bad MAC variants: wrong-length, uppercase hex, empty header, missing counter/protocol headers, counter 0, counter with leading zeros, counter > uint64 max.
- Stale (strictly lower) counter after a higher one was accepted (only the equal-counter replay is tested), and counter = UINT64_MAX followed by anything.
- Counter not consumed after a callback 503 and after a callback that throws an exception (the retry test at ~4014 covers the happy retry only); cover the exception path (`catch (...)` at line 120).
- Response MAC: a request MAC must not validate a response and vice versa (directional keys), response MAC bound to counter and status; only fixed vectors exist at 3765-3768.
- Replay-state: fingerprint mismatch at cold start (see S3), missing file for a paired workspace, oversize file (>256 bytes), duplicate keys, counter overflow text.
- Rotation/clear/reset coordination: rotation while a callback is in flight (a stale callback's advance/release is rejected by the epoch) is covered at ~3978; add clear-pairing (`setDeviceCredentials({}, ...)`) then first-device adoption.
- Malformed JSON with a valid MAC: truncated body, trailing garbage, duplicate keys, wrong types (`"queueDepth":"3"`), events > 4, lookup as a string, NUL and `\u` escapes.
- Durable-callback failure for `acceptDeviceSyncEvents` itself (DB locked or read-only) -> 503 and no counter advance; `completeDeviceSyncEvent` failure keeps the event `received` and does not hot-loop (see S2).
- Thread-safety: TSAN run of quick-label print vs. settings save (see S2).
Backup/restore:
- Data directory containing unmanaged files (see S1).
- Symlinked data directory, symlinked backup source files and manifest entry names with `..`, absolute paths, NUL (the allowlist rejects them but no test asserts it; grep finds no `create_symlink` in the tests).
- Manifest and unlisted-file rejection, missing optional entry, zero-byte inventory.db, wrong size with correct hash.
- Interrupted restore recovery from every journal state, including a stale or truncated journal file (simulating no fsync), cross-device rename failure (EXDEV hook on `renamePath`), permission denied on staging creation, and ENOSPC in `copy` (hook exists but the ENOSPC cleanup/journal path is only partially covered).
- `restoreInventatoryBackup` throwing filesystem exceptions from the throwing `exists` overloads (see S4) after activation: assert `replacementWorkspaceActiveOnFailure` and the retained journal.
- Restore keeps machine-local settings (see S3 on background flag).
- After a restore, an old-token request is rejected and the replay state is gone (end-to-end App-level test is absent).

## Quality (S4)

### [S4] Large dead legacy Scan R1 surface
- Location: src/core/scanner/InventatoryScanProtocol.cpp:73-161 (`parseQuantityRequestJson`, `parseScanRequestJson`, `parseDebugReportJson`, `parseStatusReportJson`), :365-423 (`applyDeviceQuantityCached` and cache), :425-451 (`scanResultJson`, `debugResultJson`, `quantityResultJson`, `statusResultJson` identical bodies); src/app/scanner/AppScanEnrichment.cpp:292-327 (`enqueueDeviceQuantity`, no callers); AppDeviceActions.cpp:37 (`enqueueDeviceDebug`, no callers) and the `quantities` loop at :315-374 (`deviceQuantityQueue_` is never filled)
- The server only exposes `POST /api/v1/device/sync`; everything else is unreachable except from tests. Remove or isolate; this also removes the `processDeviceRequests` quantity path that duplicates stock-mutation logic with a different save path (`saveState`) than the inbox (`completeDeviceSyncEvent`).

### [S4] Duplicate/unused helpers
- `http_server_detail::jsonEscape` (src/platform/scanner/HttpServerProtocol.cpp:19-44) is unused and lacks control-character escaping; `hexDigit`/`jsonEscape` exist twice in core/scanner (Security.cpp and JsonParser.cpp); `hexToken` duplicates `hexBytes` in Security.cpp:40/121; `InventoryTransfer.cpp` / `InventoryRestore.cpp` include a long list of unused headers (bcrypt, thread, iomanip, ...); the comment at InventoryTransferManifest.cpp:45-47 ("The PC product is Windows-only") is stale; `atomicWriteText` duplicates `writeFileAtomically` (without fsync).

### [S4] Seven copies of the same rollback-and-report block in `restoreInventatoryBackup`
- Location: src/core/transfer/InventoryRestore.cpp:164-253
- Each failure repeats `rollbackRestore`, flag clearing and error concatenation. A small lambda (`failWithRollback(primary)`) removes ~60 lines and removes the risk of one copy diverging. Same file has a duplicated `isLinkedOrReparseArtifact` check (85-98).

### [S4] Throwing `std::filesystem` overloads in the restore/recovery path
- Location: src/core/transfer/InventoryRestore.cpp:232,240; InventoryTransferRecovery.cpp:100,108,233-234,249,256,261
- `filesystem::exists(path)` without an error_code throws on EACCES/ELOOP. The outer try/catch prevents termination, but an exception thrown midway through `rollbackRestore` aborts it at an arbitrary step, leaves the journal in place and only reports "Restore filesystem error". Prefer the error_code overloads so each failure maps to the intended step error.

### [S4] `jsonMemberValuePosition` re-validates and rescans the whole body for each field (O(fields x body)); `parseDeviceSyncRequestJson` calls it ~12 times per request plus once per event field. Bounded at 64 KiB, so low impact, but a single pass into a small map would be clearer.

### [S4] Docs drift
- docs/backup-restore.md says the snapshot is consistent "while the application is using WAL", but nothing in the code enables WAL (`journal_mode` is never set); docs/backup-restore.md and docs/scanner-transport-security.md still say "Windows Credential Manager" where the Linux build uses Secret Service. docs/backup-restore.md also does not mention that restore replaces the entire data directory (see S1).

### [S4] Pre-restore backups ("Inventatory Pre-Restore <stamp>") are created in `dataPath_.parent_path()` and never pruned; each can be up to 512 MiB, and the location is outside the user's chosen backup folder (AppBackupRestore.cpp:108). Consider letting the user see/choose the location or pruning old ones.
