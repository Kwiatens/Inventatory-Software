# Slice I: Application shell, threading, settings, updater

## Coverage
Read in full: src/app.cpp, src/App.h (lines 150-1082; 1-150 are includes/helpers, skimmed via grep), src/app/shell/AppBootstrap.cpp, AppShell.cpp, AppRuntime.cpp, AppInput.cpp, AppInputModes.cpp (all but lines 45-90), AppNavigation.h, src/app/AppUpdate.cpp, src/platform/system/UpdateService.cpp (all, Linux paths in depth), src/app/settings/AppSettings.cpp/.h, AppSettingsAppearance.cpp (tail only).
Skimmed by grep only: AppShellRender.cpp (render-only, const, UI thread), AppActionSupport.h (helpers, no threading/locale hazards found).
Also read (cross-slice, because they are the other end of a crossing): src/main.cpp, src/ui/pages/settings/SettingsPageSave.cpp, src/ui/pages/onboarding/OnboardingPage.cpp (update check), BackgroundControllerLinux.cpp (start/stop/lock), StartupRegistrationLinux.cpp (tail), ConsoleLinux.cpp (spawnDetached), installer/Install-Inventatory.sh, core/storage/AtomicFile.cpp, AppDeviceActions.cpp / AppQuickLabelActions.cpp (worker-side readers of App state).
Not run: nothing built (per protocol). FTXUI behaviour checked by reading build-agent-debug/_deps/ftxui-src.

Positive notes (checked, no defect): all std::future/async results are consumed on the UI loop via processBackgroundWork; the printer worker is detached but only touches a shared_ptr completion object under its own mutex; lock order deviceQueueMutex_ -> pending->mutex is consistent; workspaceMutex_ is never held across callbacks; futures that borrow DigiKey clients are declared after the clients; run() cancels parked device requests before server_.stop(); the Linux updater verifies archive and installer SHA-256 twice (app and script), stages next to the target and uses mv -f (same-filesystem atomic rename), sets exec bit, has rollback; instance lock fd is O_CLOEXEC; version comparison implements semver precedence correctly for rc.N/numeric/alpha identifiers (tests cover rc.2>rc.1, release>rc).

## Findings

### [S2] Multi-byte UTF-8 characters typed in any text field are truncated to their first byte
- Location: src/app/shell/AppShell.cpp:94 (`translateEvent`), consumers e.g. src/app/shell/AppInputModes.cpp:100, :131, src/ui/pages/stock/StockPageInput.cpp:26, AppInputModes.cpp:40
- Category: correctness / linux-portability
- Failure scenario: User edits an item and types "µF", "Ω", "ł", "ą" etc. FTXUI delivers one `Event::Character` whose string holds the whole UTF-8 sequence ("µ" = C2 B5). `translateEvent` returns `{KeyType::Character, event.character()[0]}` (a single `char`), so only 0xC2 reaches `inputBuffer_.push_back(key.ch)`. The part name / notes / rack name / search query now contains a lone lead byte (invalid UTF-8), is committed to SQLite, exported, printed and rendered as garbage. Backspace also `pop_back()`s one byte. The app is for electronics (µ, Ω are common) and the owner/locale is Polish. HistoryPage.cpp:81 silently drops anything outside 32..126, so even there the character vanishes.
- Evidence:
  `if (event.is_character() && !event.character().empty()) { return {KeyType::Character, event.character()[0]}; }`
  `struct KeyEvent { KeyType type; char ch = '\0'; };` (Console.h:40)
  FTXUI terminal_input_parser.cpp:151 `out_(Event::Character(std::move(pending_)))` with `pending_` = whole sequence.
- Confidence: high (FTXUI source checked)
- Proposed test: feed `ftxui::Event::Character("\xC2\xB5")` through a testable `translateEvent` (move it to a header) and assert the produced key carries the full sequence; plus a handleEditValueKey test asserting the buffer equals "\xC2\xB5" and one Backspace empties it.
- Proposed fix (sketch): give `KeyEvent` a `std::string text` (or `char32_t`) for Character events; push whole code points into buffers; make Backspace pop a whole UTF-8 code point (strip continuation bytes); keep `ch` = first byte only for ASCII shortcut matching.

### [S2] Linux: disabling the background service in Settings drops the single-instance lock and signal handling of the running TUI
- Location: src/ui/pages/settings/SettingsPageSave.cpp:290 (`backgroundController_.stop()`), src/platform/system/BackgroundControllerLinux.cpp:379-390 (`stop()`); also :266 (`start()` call passes only 3 args)
- Category: linux-portability / process lifecycle
- Failure scenario: With "run in background" enabled the user opens Settings, turns it off and saves. On Windows `stop()` only tears down the tray. On Linux `stop()` joins the signal thread, calls `restoreSignalHandlers()` (SIGTERM/SIGHUP/SIGINT back to default), unlocks and closes `instanceLockFd_` (interactive.lock) and clears both callbacks, while the TUI keeps running. Consequences: (a) a second `inventatory` launch acquires the lock and opens the same workspace concurrently (two writers on inventory.db/activity.tsv/replay state, two Scan R1 servers); (b) closing the terminal (SIGHUP) or `kill` now terminates the process without the final save path in `App::run()`; (c) when enabling in-session, `start()` is re-invoked without the `onOpen` callback (run() passes it at AppShell.cpp:132-135), so the "already open, come forward" signal handler stops working for that session.
- Evidence:
  SettingsPageSave.cpp:289-291 `} else { backgroundController_.stop(); }`
  BackgroundControllerLinux.cpp:384 `flock(instanceLockFd_, LOCK_UN); close(instanceLockFd_);` inside `stop()`
  SettingsPageSave.cpp:266 `backgroundController_.start(true, false, [this] { backgroundQuitRequested_.store(true); });`
- Confidence: high on the code path; medium on the user-visible outcome of (b) (depends on FTXUI SIGHUP handling)
- Proposed test: Linux-only test of BackgroundController: acquireSingleInstance, start, stop, then assert a second controller's acquireSingleInstance still fails (i.e. a mid-session "disable" must not release the lock); or split stop() into stopTray()/stop().
- Proposed fix (sketch): add `BackgroundController::disable()` that clears `enabled_` only (Windows: remove tray), keep lock/signal thread until process exit; pass the open callback on every start().

### [S2] Linux update download has a 15 s total-transfer timeout (Windows has per-operation timeouts only)
- Location: src/platform/system/UpdateService.cpp:846 (`CURLOPT_TIMEOUT_MS, 15000L`) vs :678 (WinHttpSetTimeouts per-operation)
- Category: linux-portability
- Failure scenario: `CURLOPT_TIMEOUT_MS` bounds the whole transfer including the tar.gz asset (cap is 512 MiB, real archive is a multi-MB native binary). On any link slower than archive_size/15 s (e.g. ~1 MB/s for a 15 MB archive, mobile tether, busy Wi-Fi) curl aborts with CURLE_OPERATION_TIMEDOUT and the user sees "Could not complete the GitHub update download" on every retry, so the in-app update can never succeed. The release-metadata call (8 s total, :453) is fine for small JSON. The progress bar UI explicitly supports long downloads (ETA), so a hard 15 s cap contradicts it.
- Evidence: `curl_easy_setopt(request, CURLOPT_TIMEOUT_MS, 15000L);` in the download path.
- Confidence: high on code; medium on impact (depends on archive size and link)
- Proposed test: none practical without a local HTTPS server; at least a comment/test documenting the policy. Could factor the option setup into a function and assert CURLOPT_TIMEOUT_MS is unset / LOW_SPEED_* used (unit test via curl_easy_getinfo is not available, so manual verification with `tc`/slow server).
- Proposed fix (sketch): drop CURLOPT_TIMEOUT_MS for downloads; use `CURLOPT_LOW_SPEED_LIMIT=1024` + `CURLOPT_LOW_SPEED_TIME=15` (stall detection) like WinHTTP's per-read timeouts.

### [S2] Listening/accepted sockets of the Scan R1 bridge are inherited by long-lived children (xdg-open, browser, update installer)
- Location: src/platform/scanner/HttpServerLifecycle.cpp:144 (`::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`), HttpServer.cpp:98 (`accept(`), consumed by src/platform/system/ConsoleLinux.cpp:97-112 (`spawnDetached` -> xdg-open), src/platform/system/UpdateService.cpp:970-996 (fork+execl installer), MdnsService.cpp:257 (avahi-publish-service)
- Category: linux-portability (fd inheritance; sibling of the CLOEXEC handling already done for the lock fd and the handshake pipe)
- Failure scenario: User presses the "open link/datasheet" action while the bridge is running. `spawnDetached` forks and `execlp("xdg-open")`; xdg-open execs the browser (which outlives it). The listening socket has no CLOEXEC so the browser holds a duplicate of the bridge's listening fd. Later the user changes the port / data folder / re-pairs: `server_.stop()` closes the app's copy but the browser's copy keeps the socket in LISTEN state, so `server_.start(samePort)` fails with EADDRINUSE ("Scan R1 service failed to restart") and the scanner's connections are accepted by the kernel and never answered, until the browser exits. avahi-publish-service and the updater's waiter shell inherit the same fds (the updater stops the server before fork, but accepted client sockets and any other fd still leak).
- Evidence: no `SOCK_CLOEXEC`/`accept4` anywhere under src/ (`grep -rn "SOCK_CLOEXEC\|FD_CLOEXEC" src` only finds UpdateService.cpp:957); child code in spawnDetached only does `dup2` of 0/1/2 and `execlp`.
- Confidence: high that fds are inherited; medium-high on the xdg-open persistence scenario (browser launched by xdg-open is long-lived by definition)
- Proposed test: Linux test that opens a LocalHttpServer on an ephemeral port, runs `spawnDetached("sleep", "2")`-style child (or fork+exec of /bin/sleep), stops the server and asserts a new server can bind the same port immediately.
- Proposed fix (sketch): `socket(..., SOCK_STREAM | SOCK_CLOEXEC, ...)` and `accept4(..., SOCK_CLOEXEC)` on Linux (guard with #ifndef _WIN32 in SocketPlatform.h); in fork children additionally close fds >= 3 (`close_range(3, ~0U, 0)` with fallback) before exec.

### [S3] Downloaded update package (archive + checksums + installer script) is never deleted after a successful update, nor after a failed hand-off or on quit
- Location: src/app/AppUpdate.cpp:401-448 (`prepareUpdateHandoff`), installer/Install-Inventatory.sh:30-32 (`cleanup` only removes `$stage`), src/app/AppUpdate.cpp:110-124
- Category: correctness / resource leak
- Failure scenario: (1) Successful update: the app hands the verified package in `/tmp/Inventatory-update-<pid>-<tick>/` to the installer and exits; `Install-Inventatory.sh` `cleanup()` only `rm -rf "$stage"` (the staging dir beside the binary). Nothing removes the temp dir, so every update leaves the archive in /tmp. (2) `prepareUpdateHandoff` failure branches (saveState failure, marker write failure, `launchUpdateInstaller` failure, lines 421-444) set Failed but never call `removeUpdateDownloadDirectory`; Retry then does `updatePackage_.reset()` (line 468), losing the path forever. (3) Quitting while Downloading/Verifying: the future is not cancelled (see next finding) and no completion handler runs to remove the directory.
- Evidence: `cleanup() { if [ -n "$stage" ] && [ -d "$stage" ]; then rm -rf -- "$stage"; fi }`; no `rm` in the script touches `dirname "$archive_path"`.
- Confidence: high
- Proposed test: extend Test-InventatoryLinuxInstall.sh (update mode) to assert the download directory is gone after a successful `--update` run; unit-test that prepareUpdateHandoff failure removes the directory (needs seam).
- Proposed fix (sketch): in update mode, script removes `dirname "$archive_path"` after writing the `complete` marker (only if basename starts with `Inventatory-update-`); app removes `downloadDirectory` on every Failed transition and in ~App/run() shutdown when no installer was launched. Also sweep stale `Inventatory-update-*` older than a day at startup.

### [S3] Quitting during an in-flight update/firmware/update-check operation blocks shutdown and is not cancelled
- Location: src/app/shell/AppShell.cpp:174-180 (shutdown sequence), src/App.h:1052-1059 (futures), src/main.cpp:81-87 (`_Exit` only for `workAbandoned()`)
- Category: concurrency / process lifecycle
- Failure scenario: User starts an update, presses quit during Downloading (requestUserExit has no update guard). `run()` stops workspace-bound work with a 3 s deadline but `updateOperationFuture_`, `updateCheckFuture_`, `scanFirmwareFuture_` are not included and `updateDownloadState_->cancelRequested` is never set. `~App` then destroys a `std::future` from `std::async(launch::async)`, which joins: exit waits for the download (Windows: unbounded by total time; Linux: up to the 15 s curl timeout; update check: up to 8 s). The terminal appears hung and a launcher waiting for the lock may time out. The temp dir leaks (previous finding).
- Evidence: `stopWorkspaceBoundWorkUntil` handles only digiKeyRefreshFuture_/importSyncFuture_/scanDigiKeyEnrichmentFuture_/bomEnrichmentFuture_.
- Confidence: high
- Proposed test: seam-level test with a fake downloader that blocks until cancelRequested; assert run()-shutdown helper sets the flag and returns within the deadline.
- Proposed fix (sketch): in run() before the final save: set `updateDownloadState_->cancelRequested`, wait with the same deadline, on timeout set `workAbandoned_` so main `_Exit`s; remove the temp directory.

### [S3] Settings save reports "device bridge could not restart" when the scanner is simply not paired
- Location: src/ui/pages/settings/SettingsPageSave.cpp:257-262 and :311-316; src/app/shell/AppRuntime.cpp:433-442
- Category: correctness (misleading status; AGENTS.md requires visible and truthful bridge status)
- Failure scenario: Workspace not paired (`setupComplete == false` or token empty). User changes the port or data folder and saves. `restartDeviceService()` stops the server, sets "Scan R1 service is disabled until this workspace is paired" and returns without starting. Then `bridgeRestarted = server_.running()` is false and the final `setMessage` overwrites the accurate text with a Warning "Settings saved, but the device bridge could not restart". Same wrong message after a data-folder switch to a workspace that has no pairing yet.
- Evidence: `restartDeviceService(); bridgeRestarted = server_.running();` ... `bridgeRestarted ? "Settings saved; device bridge restarted" : "Settings saved, but the device bridge could not restart"`.
- Confidence: high
- Proposed test: refactor the message choice into a pure helper `(paired, running, portChanged, dataChanged) -> message` and unit test the unpaired case.
- Proposed fix (sketch): have `restartDeviceService()` return an enum {Restarted, DisabledUnpaired, Failed}; map to messages; do not overwrite the unpaired notice.

### [S3] Quick-label rollback assigns `settings_` unlocked while the HTTP bridge is serving, and discards the user's whole draft
- Location: src/ui/pages/settings/SettingsPageSave.cpp:229-243 (non-dataChanged rollback), cf. :172 and :206 (server stopped there, safe)
- Category: concurrency / error recovery
- Failure scenario: `saveQuickLabels` fails (disk full / read-only). The rollback runs `settings_ = oldSettings; settingsDraft_ = oldSettings;` with the bridge still running (dataChanged is false so the server was never stopped). `handleDeviceSync` on an HTTP worker reads `settings_.quickLabelPresets` under `quickLabelMutex_` (AppDeviceActions.cpp:69-72), but this writer does not take that lock: a vector/string copy-assign racing a worker's copy is UB. Separately `settingsDraft_ = oldSettings` throws away every other staged edit (colours, DigiKey fields, printer) so the user must redo the whole form after a transient write error.
- Evidence: `settings_ = oldSettings; settingsDraft_ = oldSettings;` outside any `lock_guard<mutex> lock(quickLabelMutex_)` (the locked write is at :249-251).
- Confidence: medium (race window tiny; draft loss certain)
- Proposed test: unit test of a saveSettingsDraft seam with failing saveQuickLabels asserting the draft is preserved and settings_ equals old.
- Proposed fix (sketch): take quickLabelMutex_ for every `settings_` mutation that can overlap a running server (or keep presets in their own mutex-guarded object); on rollback keep `settingsDraft_` (only restore `settings_`), leave `settingsDirty_ = true`.

### [S3] Worker thread reads UI-owned `printerService_` without synchronisation
- Location: src/app/labels/AppQuickLabelActions.cpp:133 (`printerService_.configuredPrinter()`), src/label_printer/platform/LabelPrinterPlatform.cpp:262 (returns `const string&`); writers SettingsPageSave.cpp:~296 `printerService_.setConfiguredPrinter(...)`
- Category: concurrency
- Failure scenario: `printDeviceQuickLabel` runs on an HTTP worker (called from handleDeviceSync). It copies `printerService_.configuredPrinter()` while the UI thread may be inside `setConfiguredPrinter(settings_.printerQueue)` (settings save with a changed printer) - concurrent std::string read vs move-assign. App.h:433-435 states workers never touch the UI-owned service; this call breaks that invariant.
- Evidence: `string printerName = printerService_.configuredPrinter();` in a function reachable only from the device callback.
- Confidence: medium (small window)
- Proposed test: none deterministic; TSAN run with a scripted sync + settings save.
- Proposed fix (sketch): mirror the configured printer name into a mutex-protected snapshot (the existing workspace/quickLabel mutex) updated by the UI thread, and read that in the worker.

### [S3] Update-check cadence breaks when the stored timestamp is in the future
- Location: src/platform/system/UpdateService.cpp:478-480
- Category: correctness
- Failure scenario: The system clock was once ahead (wrong RTC/NTP fix, VM restore). `last_update_check_unix_seconds` is saved as, say, now+30 days. After the clock is corrected `nowUnixSeconds - lastCheckUnixSeconds` is negative, `>= 86400` is false, so automatic checks are skipped until the real time catches up (up to the size of the skew). Settings loader accepts any non-negative value.
- Evidence: `return enabled && (lastCheckUnixSeconds <= 0 || nowUnixSeconds - lastCheckUnixSeconds >= kUpdateCheckIntervalSeconds);`
- Confidence: high
- Proposed test: `assert(isUpdateCheckDue(true, now + 1000, now))`.
- Proposed fix (sketch): treat `last > now` as due (or clamp).

### [S3] Server stop on the UI thread without first failing parked device requests (inconsistent with the shutdown path)
- Location: src/app/shell/AppRuntime.cpp:433-435 (`restartDeviceService`), src/app/AppUpdate.cpp:418-420, SettingsPageSave.cpp:119-120, AppBackupRestore.cpp:96-97
- Category: concurrency / responsiveness
- Failure scenario: `App::run()` (AppShell.cpp:170-175) documents that `server_.stop()` joins workers that wait for the UI thread and therefore calls `cancelPendingDeviceRequests` first. All other callers (port change, data-folder switch, restore, update hand-off) run on the UI thread and call `server_.stop()` directly. If a scanner sync is parked in `enqueueDeviceQuantity` the UI thread blocks inside stop() until the worker's 3 s `wait_for` expires (AppScanEnrichment.cpp:320): a 3 s frozen terminal on every such operation (not a permanent deadlock only because of that timeout).
- Evidence: `mdnsService_.stop(); server_.stop();` at the top of restartDeviceService without `cancelPendingDeviceRequests(...)`.
- Confidence: medium (depends on LocalHttpServer::stop joining workers; comment in run() says it does)
- Proposed test: with a fake pending request parked, assert a helper `stopDeviceService()` returns promptly.
- Proposed fix (sketch): one `stopDeviceService()` helper = cancelPendingDeviceRequests(reason,false) + mdns stop + server stop, used by all call sites.

### [S3] Linux desktop launcher is recreated on every launch and retargeted by any build
- Location: src/app/shell/AppShell.cpp:112-115, src/platform/system/StartupRegistrationLinux.cpp (`createDesktopShortcut`, ~lines 302-345)
- Category: linux-portability (non-idempotent startup side effect; contrast with setBackgroundStartupEnabled which keeps `registeredExecutable()` and rewrites only when changed)
- Failure scenario: (a) The file writes are content-checked (`writeFileIfChanged`), but a user who deletes `~/Desktop/inventatory.desktop` gets it back on every start. (b) Launching a development build or a second install from another path rewrites `~/.local/share/applications/inventatory.desktop` and the Desktop copy to point at that binary (the systemd unit has an explicit guard against exactly this, the launcher has none). (c) Icons are re-checked on every launch.
- Evidence: `const auto executable = currentExecutablePath();` ... `writeFileIfChanged(launcher, contents, ...)` with no registered-executable check; called whenever `completedOnboardingVersion >= 1` at every interactive start.
- Confidence: high
- Proposed test: Linux test with temp XDG dirs: create shortcut, delete the Desktop copy, run again, assert it is not recreated; create from exe A then call from exe B and assert the launcher still names A when A exists.
- Proposed fix (sketch): create the Desktop copy only at onboarding/first creation (record a flag), reuse the registeredExecutable-style guard for the applications entry.

### [S3] Update hand-off has no pre-flight check that the install directory is writable
- Location: src/app/AppUpdate.cpp:401-448 vs installer/Install-Inventatory.sh:98 and :126
- Category: correctness / UX
- Failure scenario: Linux install in a root-owned path (e.g. /usr/local/bin, /opt) run by a normal user. The app downloads and verifies the package, stops the bridge, saves, launches the installer and exits (TUI disappears). Only then does `mktemp -d "$(dirname target)/.inventatory-update.XXXXXX"` fail ("installation folder is not writable"), the marker becomes `failed`, and the waiter relaunches the old binary showing the failure. The user lost the session and wasted the download for something detectable up front.
- Evidence: `stage=$(mktemp -d "$(dirname -- "$target")/.inventatory-update.XXXXXX") || fail 'The installation folder is not writable'` runs after the app already exited.
- Confidence: high
- Proposed test: unit test of a `canReplaceExecutable(path)` helper (access(dirname, W_OK) on temp dirs).
- Proposed fix (sketch): in `beginSoftwareUpdate` (Linux) check `access(dirname(/proc/self/exe), W_OK)` and show "this installation is not user-writable; update via your package manager / reinstall" instead of starting.

### [S3] Update temp directory is predictable and created with default permissions in a shared temp dir
- Location: src/app/AppUpdate.cpp:110-118, :223
- Category: security (hardening)
- Failure scenario: Path is `$TMPDIR/Inventatory-update-<pid>-<steady_clock ticks>`; `create_directories` succeeds silently if another local user pre-created it and mode is 0755 otherwise. A hostile local user who guesses/creates the directory could swap files between verification (`beginUpdateVerification`) and the installer launch (the installer re-verifies the archive and itself against the manifest, so the practical impact is limited to the manifest file itself being replaced - but `$checksums_path` is not re-verified against anything). Low likelihood; cheap to remove.
- Evidence: `filesystem::temp_directory_path() / ("Inventatory-update-" + to_string(process) + "-" + to_string(tick))`.
- Confidence: low-medium
- Proposed test: assert directory mode 0700 and that creation fails if it already exists.
- Proposed fix (sketch): `mkdtemp` (0700) on Linux; use the user's runtime/cache dir; fail if the directory already exists.

### [S3] Release checksums provide integrity only (same-origin manifest, no signature)
- Location: src/app/AppUpdate.cpp:286-320, src/platform/system/UpdateService.cpp:889-902, installer/Install-Inventatory.sh:42-49
- Category: security (design note)
- Failure scenario: Archive, `SHA256SUMS-linux.txt` and the installer script are all fetched from the same GitHub release; an attacker who can publish/replace release assets (compromised token) can replace all three consistently and the app will execute the new installer via `sh` and replace the binary. The code and docs (docs/scanner-transport-security.md style) should state that the trust root is the GitHub release account, or add a detached signature verified with an embedded public key.
- Evidence: all three assets come from `buildReleaseAssetUrl(Inventatory_RELEASE_REPOSITORY, tag, asset)`; no signature step.
- Confidence: medium (design decision, not a bug)
- Proposed test: n/a (docs) or negative test with a swapped manifest once signatures exist.
- Proposed fix (sketch): document it; optionally ed25519 signature over the manifest verified in-app.

### [S3] writeFileAtomically (Linux): no directory fsync, EINTR treated as failure, replaces symlinked config with a regular file
- Location: src/core/storage/AtomicFile.cpp:~113-133 (`writePosixFile`), :~200 (`filesystem::rename`)
- Category: persistence / linux-portability
- Failure scenario: (1) After `rename()` the parent directory is not fsync'd, so a crash/power loss right after a settings or marker write may leave the old file (Windows uses MOVEFILE_WRITE_THROUGH, so the guarantee differs). (2) `write()` returning -1/EINTR is handled as "Unable to write temporary file" (the loop checks `written <= 0`), e.g. when a signal handler (SIGUSR1/SIGTERM handlers are installed on Linux) interrupts a write; settings save fails spuriously. (3) Users who keep `~/.config/Inventatory/settings.conf` as a symlink into a dotfiles repo lose the symlink on the first save.
- Evidence: `if (written <= 0) { setError(... "Unable to write temporary file" ...); success = false; break; }` and no `fsync` of the parent after `filesystem::rename`.
- Confidence: medium (sigaction flags are `sa_flags = 0`, no SA_RESTART, at BackgroundControllerLinux.cpp:192, so a SIGUSR1/SIGTERM delivered to the writing thread interrupts write()/fsync and the failure is real but rare; the signal may also land on another thread)
- Proposed test: tests for writeFileAtomically are absent (see gaps); add temp-dir tests for overwrite, collision of temp name, read-only directory error, and symlink behaviour.
- Proposed fix (sketch): retry on EINTR; fsync the parent dir fd after rename; if destination is a symlink, resolve it first (`std::filesystem::canonical`) and write the target.

## Test gaps
- `launchUpdateInstaller` (Linux fork/handshake: setsid failure, exec failure -> returns false and reaps child; invalid releaseVersion rejected) -> test with a fake installer script (`exit 0`, nonexistent path, script that sleeps) and assert return values/zombie reaping.
- `relaunchAfterUpdate` (waiter script: pending -> complete transition, 120 s fallback, paths with spaces/quotes) -> test the waiter script with a temp marker and a stub executable, using fork so the test process is not replaced.
- `downloadReleaseAsset` negative paths on Linux (cancel via progress, oversize Content-Length, truncated body, non-2xx, partial file removal) -> local TLS test server or inject a curl-free transport seam.
- `writeFileAtomically` has no direct tests (temp collision, read-only dir, overwrite, symlink).
- `appSettingsDirectory()` XDG rules (relative XDG_CONFIG_HOME ignored, HOME fallback, cwd fallback) and `discoverInventatoryDataPath()` / `documentsInventatoryPath()` (XDG_DATA_HOME, ~/Documents legacy candidate) -> temp HOME/XDG env tests.
- `isUpdateCheckDue` with `last > now` (future timestamp).
- `isVersionNewer` / `validReleaseTag` edge cases not asserted: "v1.2.3-" (empty prerelease), "v1.2.3." (trailing dot), "1.0.0-rc.1" vs "1.0.0-rc.1.1", numeric vs alpha identifiers ("rc.1" vs "rc.beta"), "0.1.2+build" installed string, "dev" installed (never newer).
- `translateEvent` multi-byte input (see S2) and Backspace over multi-byte text.
- Settings workflow: `saveSettingsDraft` rollbacks (quick-label save failure, workspace activation failure), bridge-restart message when unpaired, and `backgroundController_.stop()` lock semantics on Linux have no tests.
- Shutdown with an in-flight update download (cancel + temp cleanup).
- Update marker parsing (`loadUpdateCompletionMarker`): pending/failed/complete, oversized marker/notes, CRLF, missing notes.

## Quality (S4)

### [S4] processUpdateCheck / processScanFirmwareCheck call `future.get()` without exception handling
- Location: src/ui/pages/onboarding/OnboardingPage.cpp:476-480, src/ui/pages/scanner/InventatoryScanSetupPage.cpp:169-173
- Category: quality
- Detail: `processSoftwareUpdate` wraps `get()` in try/catch, these two do not; a `std::bad_alloc` from JSON parsing in the worker propagates out of the FTXUI event callback and terminates the process. Cheap to make consistent.
- Confidence: low (only allocation failures can throw)

### [S4] `saveAppSettings` discards the specific error from `writeFileAtomically`
- Location: src/app/settings/AppSettings.cpp:~398-400 (`string error; return writeFileAtomically(path, text, &error);`)
- Detail: every caller shows a generic "Unable to save Inventatory settings"; disk-full vs permission vs read-only is lost. Same in `saveQuickLabels`. Consider an optional `std::string* error` parameter.

### [S4] Version parser accepts "v1.2.3." and "v1.2.3-" as valid release tags
- Location: src/platform/system/UpdateService.cpp:138-160 (`versionParts` uses getline, which never yields the trailing empty token; `versionPrerelease` returns "" after a trailing '-')
- Detail: harmless today (tag must also match html_url) but lenient parsing in a security-adjacent validator; add `normalized.back() != '.'` and reject a '-' with empty prerelease.

### [S4] Releases are chosen with `?per_page=1` (newest created, not highest version)
- Location: src/platform/system/UpdateService.cpp:352 / :443
- Detail: a backport release (e.g. v0.1.3 published after v0.2.0) becomes "latest", hiding v0.2.x updates from 0.2.0 users, and every user on stable is offered the newest prerelease. If deliberate (comment says so) document it; otherwise fetch a small page and pick the max valid tag by `isVersionNewer`.

### [S4] Duplicated asset-name constants and `readBoundedFile` reads before bounding
- Location: AppUpdate.cpp:37-47 duplicates UpdateService.cpp:30-38; AppUpdate.cpp:53-59 loads the whole file then checks `size() <= maximum`.
- Detail: expose `kApplicationArchiveName` etc. from UpdateService.h; check `file_size` (or read max+1 bytes) before loading.

### [S4] Dead/unused state and over-long constructor
- Location: App.h:268 (`UpdateDownloadState::verifying` written, never read), src/app.cpp:52-171 (one ~120-line constructor mixing settings recovery, workspace activation, scanner server start and update check)
- Detail: remove the unused flag; extract `startDeviceServiceIfEligible()` (the same sequence is duplicated almost verbatim in `restartDeviceService`, AppRuntime.cpp:433-459) so the pairing/credential/mDNS logic exists once.
