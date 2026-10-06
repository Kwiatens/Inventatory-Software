# Slice W: Windows platform layer (read-only review, Windows cannot be compiled here)

## Coverage
Read in full: `src/platform/system/{BackgroundController.cpp,.h, BackgroundLauncher.cpp, StartupRegistration.cpp,.h, Console.cpp, Console.h, Environment.cpp, UpdateService.cpp, *.rc}`,
`src/platform/security/CredentialStore.cpp`, `src/platform/scanner/{BleProvisioningService.cpp, HttpServer.cpp, HttpServerLifecycle.cpp, HttpServerConnection.cpp, MdnsService.cpp/.h, SocketPlatform.h}`,
`src/platform/digikey/{DigiKeyTransport.cpp, DigiKeyApiPrivate.h}` (+ diff of b6c9be6), `src/label_printer/platform/LabelPrinterWindows.cpp` and the Windows blocks of `LabelPrinterPlatform.cpp`,
`src/core/storage/AtomicFile.cpp`, `src/core/scanner/InventatoryScanProtocolSecurity.cpp`, Windows blocks of `core/transfer/InventoryTransferOps.cpp`, `app/AppUpdate.cpp`, `app/shell/{AppShell,AppBootstrap,AppRuntime}.cpp`, `app/settings/AppSettings.cpp` (appSettingsDirectory), `main.cpp`, `installer/*.ps1 + *.cmd`, `CMakeLists.txt` platform selection.
All other `_WIN32` sites enumerated with grep (`localtime_s` wrappers, UI text branches) were checked and are fine. Not read in depth: `InventoryTransferValidation.cpp` (T13), `Test-InventatoryUpdate.ps1` (only skimmed), `ftxui` integration.

### DigiKey regression check (b6c9be6): no Windows build regression found
- CMake still selects `DigiKeyTransport.cpp` (WinHTTP) for `WIN32` (CMakeLists.txt:46); `winhttp` stays in both link lines (:236, :327). libcurl is only `find`-ed in the non-Windows branch (:56), so Windows does not need it.
- `DigiKeyTransport.cpp` still defines every symbol declared in `DigiKeyApiPrivate.h` (`widen`, `narrow`, `requestHttp`, `isSafeHeaderValue`, ...); `HttpResponse` uses `uint32_t`, which the Windows file assigns from `DWORD` without trouble.
- The removed `#ifdef _WIN32` guards in `DigiKeyApi.cpp / DigiKeyJson*.cpp / DigiKeyProduct*.cpp` only deleted the Linux stubs; the Windows code path is unchanged. The test guard removal in `testPackageGHardening` is consistent (the Linux transport implements the same helpers).
- Nit: `DigiKeyTransport.cpp:3-4` defines `WIN32_LEAN_AND_MEAN`/`NOMINMAX` unguarded, which would warn (C4005) if the command line ever defines them. Harmless today.

## Findings

### [S2] Environment variables are read through the ANSI code page, so non-ASCII Windows profile paths resolve to the wrong place
- Location: src/platform/system/Environment.cpp:13-19 (callers app/shell/AppBootstrap.cpp:16,37; app/settings/AppSettings.cpp:169,172)
- Category: correctness
- Failure scenario: a user whose profile or OneDrive path contains characters outside the system ANSI code page (for example `C:\Users\Łukasz` on a Western-European ACP 1252, or a Cyrillic/Greek/CJK user name on an en-US system). `_dupenv_s` returns the CRT's narrow copy of the environment, which is built with WideCharToMultiByte(CP_ACP) and is lossy (best-fit or `?`). `filesystem::path(*profile)` then decodes it as ACP again, giving `C:\Users\Lukasz\Documents\Inventatory` or a path containing `?`. `appSettingsDirectory()` and `documentsInventatoryPath()` point to a directory that does not exist (a standard user cannot create it under `C:\Users`) or is a different folder, so settings cannot be saved, or an empty workspace is used instead of the existing one. The code elsewhere is careful about UTF-8 (SQLite path, settings value via `u8path`), but the bootstrap of both locations goes through this function. The test (tests/inventatory_tests.cpp:1668) only uses an ASCII value.
- Evidence: `if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) return std::nullopt; std::string value(raw);` and `filesystem::path(*value) / "Inventatory"`.
- Confidence: high on the mechanism; the exact outcome depends on ACP and name.
- Proposed test: Windows-only test that sets a wide env var with a character not in the ACP and checks that `appSettingsDirectory()` / `documentsInventatoryPath()` return a path whose `wstring()` contains it.
- Proposed fix (sketch): add `std::optional<std::filesystem::path> environmentPath(const wchar_t*)` using `GetEnvironmentVariableW` (or `_wdupenv_s`) and use it for `LOCALAPPDATA`, `USERPROFILE`, `OneDrive*`; or use `SHGetKnownFolderPath(FOLDERID_Documents / LocalAppData)`, which also fixes redirected Documents folders. A cheap global mitigation is an application manifest with `<activeCodePage>UTF-8</activeCodePage>` (Win10 1903+) plus `/utf-8` (see the build finding).

### [S2] File, save and folder dialogs use the ANSI APIs, so paths with characters outside the ACP cannot be chosen
- Location: src/platform/system/Console.cpp:116-127 (`GetOpenFileNameA`), :137-152 (`GetSaveFileNameA`), :166-172 (`SHBrowseForFolderA`/`SHGetPathFromIDListA`)
- Category: correctness
- Failure scenario: the user selects a CSV, BOM, backup folder or data folder whose path contains a letter outside the ACP (a Polish folder on a Western ACP, Cyrillic on en-US). The dialog hands back `?`/best-fit characters; `filesystem::path(fileName)` then names a nonexistent file or invalid path, so the import, export, backup or data folder switch fails ("file not found", or a staged data directory that cannot be created). The buffers are `MAX_PATH` char arrays, so long paths are truncated as well. `stageInventatoryFolder` then stores that mangled path in `settingsDraft_.dataDirectory`.
- Evidence: `char fileName[MAX_PATH] = {}; OPENFILENAMEA dialog{}; ... GetOpenFileNameA(&dialog) ... selectedPath = filesystem::path(fileName);`
- Confidence: high
- Proposed test: manual Windows check plus a unit test of a helper that converts the selection from wide to `filesystem::path`.
- Proposed fix (sketch): use `OPENFILENAMEW`, `BROWSEINFOW` + `SHGetPathFromIDListEx`/`IFileDialog` (also removes the MAX_PATH limit); convert titles and filters with the existing UTF-8 widen helper. `SHBrowseForFolder` with `BIF_NEWDIALOGSTYLE` also requires COM to be initialized on the calling thread and nothing in the main thread does that (`CoInitialize` appears only in StartupRegistration/BLE), so the dialog silently falls back to the old style; wrap with a `ComApartment`.

### [S2] BLE provisioning runs synchronously on the UI thread for up to minutes
- Location: src/ui/pages/scanner/InventatoryScanSetupPage.cpp:251 (calls `BleProvisioningService::provision`, src/platform/scanner/BleProvisioningService.cpp:146-383)
- Category: concurrency
- Failure scenario: `provision()` does `FromBluetoothAddressAsync(...).get()`, `PairAsync(...).get()` (blocks until the user answers the Windows pairing prompt), up to 3x GATT discovery with 500 ms sleeps, an optional unpair/re-pair, and a 25 s `wait_for` on the confirmation. All of it runs inside `provisionSelectedBleSetupDevice()` on the foreground App thread, so the TUI stops redrawing and ignores keys, mouse and resize, `processBackgroundWork()` stops (device requests parked for the UI loop time out, HTTP workers wait), and a hung BLE stack freezes the whole app with no cancel. AGENTS.md requires slow work to be done in workers and queued back to the UI.
- Evidence: `const auto outcome = bleProvisioning_.provision(request, error);` (no future/async around it) and `statusChanged.wait_for(lock, chrono::seconds(25), [&] { return terminal; });`
- Confidence: high that it blocks; the same call site is shared with Linux, so the Linux backend is affected too.
- Proposed test: stub backend whose `provision` sleeps, assert the UI tick still runs (needs an injectable backend).
- Proposed fix (sketch): run `provision` in `std::async` stored as a future on `App`, show a "waiting for scanner" step, apply the result in the next tick, and allow Esc to cancel (set a cancel flag checked between steps).

### [S2] `SO_REUSEADDR` on Windows defeats the port-in-use detection of the Scan R1 listener
- Location: src/platform/scanner/HttpServerLifecycle.cpp:149-154 (bind loop at :50-72)
- Category: security
- Failure scenario: on Windows `SO_REUSEADDR` does not mean "rebind after TIME_WAIT"; it lets a second socket bind the same address and port while another one is listening, unless the first used `SO_EXCLUSIVEADDRUSE`. The single-instance mutex is `Local\` (per session), so a second logon session of the same user (fast user switching, RDP) or any other local program using the same private IP and port can bind successfully instead of failing, and the `preferredPort..+19` fallback never moves on. The scanner (and the mDNS record) then reach one of two listeners nondeterministically, one of which has different credentials, so requests are rejected as unauthorized or a foreign process receives the authenticated requests (integrity is protected by HMAC, but availability is not, and the traffic is not confidential). Linux behaviour (REUSEADDR only affects TIME_WAIT) is what the code assumes.
- Evidence: `BOOL reuse = TRUE; setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));`
- Confidence: medium (Winsock semantics are well documented; the multi-session case was not run)
- Proposed test: Windows-only test binding two `LocalHttpServer` instances to the same preferred port and expecting the second to pick `port+1`.
- Proposed fix (sketch): on `_WIN32` set `SO_EXCLUSIVEADDRUSE` instead of `SO_REUSEADDR` (listening sockets can be re-bound immediately after close on Windows).

### [S2] Hide-to-tray and restore depend on `GetConsoleWindow()`, which does nothing under Windows Terminal
- Location: src/platform/system/BackgroundController.cpp:99-105 (`restoreConsole`), :362-366 (`hideConsole`); installer/Install-Inventatory.ps1:145-165 (launches in `wt.exe` when present)
- Category: correctness
- Failure scenario: when the console is hosted by Windows Terminal (the default terminal on current Windows 11 and what the update installer prefers), `GetConsoleWindow()` returns the hidden pseudo-console window, so `ShowWindow(console, SW_HIDE)` hides nothing and `SetForegroundWindow` focuses nothing. With "Background & startup" enabled, pressing Esc/q is documented to hide the app in the notification area, but the terminal tab simply stays on screen; the tray "Open" does not bring anything forward; closing the tab terminates the process (and the scanner service) because the console is gone. The classic conhost path used by `run.ps1` works, so this shows up only for installed builds.
- Evidence: `if (const HWND console = GetConsoleWindow(); console != nullptr) ShowWindow(console, SW_HIDE);`
- Confidence: medium (known Windows Terminal behaviour; needs a Windows run to confirm the user-visible effect)
- Proposed test: manual matrix: installed build under conhost and Windows Terminal, Esc with background enabled.
- Proposed fix (sketch): detect that the console window is not visible/top-level (`IsWindowVisible` false, or `GetConsoleWindow` owner is the PseudoConsoleWindow class) and fall back to quitting the foreground (the service restarts through `restartAsBackgroundService`), or start the TUI with `CREATE_NEW_CONSOLE` under conhost explicitly.

### [S3] `path::string()` throws on MSVC for non-ACP paths and is used in error and bookkeeping paths
- Location: app/bom/AppBomProjectActions.cpp:154 (`project.sourcePath = sourcePath.string();`), app/workspace/AppWorkspace.cpp:219, app/persistence/AppPersistence.cpp:42,51,69,85,98, app/persistence/AppHistoryPersistence.cpp:28,38,69, app.cpp:56, core/transfer/InventoryTransferOps.cpp:56,66,75,... , InventoryTransferManifest.cpp:181-216, InventoryTransfer.cpp:50,69, InventoryBackup.cpp:47
- Category: correctness
- Failure scenario: MSVC's `std::filesystem::path::string()` converts with the ACP and throws `std::system_error` when a character cannot be represented. The project already moved the SQLite and settings paths to `u8string()` so Unicode data directories are meant to work, but these sites (many in failure or message paths) still call `.string()`. A data folder containing, for example, `ł` on a Western ACP makes "Loaded Inventatory folder: ..." or an import error message throw, and a thrown exception in the UI loop ends the process (BOM import stores `sourcePath` the same way).
- Evidence: `project.sourcePath = sourcePath.string();` / `error = "Unable to rename " + source.string() + ...`
- Confidence: medium (relies on MSVC STL throwing instead of substituting; I could not run it)
- Proposed test: Windows-only test calling the import and transfer failure paths with a path containing U+0141 under a non-1250 ACP.
- Proposed fix (sketch): add one `displayPath(const path&)` helper returning `u8string()` and use it everywhere; or set the process ACP to UTF-8 via manifest (`activeCodePage`) so `.string()` is UTF-8.

### [S3] WinHTTP sessions use `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`, which ignores the user's proxy configuration
- Location: src/platform/digikey/DigiKeyTransport.cpp:166, src/platform/system/UpdateService.cpp:342,675
- Category: correctness (Windows/Linux asymmetry)
- Failure scenario: `DEFAULT_PROXY` reads only the machine-wide WinHTTP proxy (`netsh winhttp`), not the per-user Internet Options / PAC / WPAD proxy (the documented replacement on Windows 8.1+ is `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`). Users behind a corporate or school proxy get "Unable to connect to DigiKey" / "Could not read the latest GitHub release" while a browser works. The Linux libcurl path honours `https_proxy`/`no_proxy`.
- Evidence: `WinHttpOpen(L"Inventatory updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)`
- Confidence: medium
- Proposed test: none practical (network); document manual check behind a proxy.
- Proposed fix (sketch): use `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` (fallback to DEFAULT_PROXY when the flag is unsupported) in a shared `openWinHttpSession()` helper used by both files (also removes ~60 lines of duplicated handle plumbing).

### [S3] Scan R1 pairing token is copied to the clipboard as ANSI `CF_TEXT` with no clipboard-history exclusion
- Location: src/platform/system/Console.cpp:79-109 (`copyToClipboard`), used at ui/pages/scanner/InventatoryScanSetupPage.cpp:146
- Category: security
- Failure scenario: `copyInventatoryScanToken()` puts the shared HMAC secret on the clipboard. On Windows 10/11 with Clipboard History (Win+V) or cloud clipboard sync enabled, the secret is stored in history and may be synced to the user's other devices, and it stays on the clipboard indefinitely. AGENTS.md says the scanner secret must not leak outside the credential store. Separately `CF_TEXT` is the ANSI format, so any non-ASCII text copied through this helper is mangled; use `CF_UNICODETEXT`.
- Evidence: `if (SetClipboardData(CF_TEXT, handle) == nullptr) {`
- Confidence: medium (user-initiated action, but the mitigation is cheap)
- Proposed test: manual; unit-test the helper with non-ASCII text.
- Proposed fix (sketch): register `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory` (DWORD 0) and `CanUploadToCloudClipboard` (DWORD 0) formats alongside the data, use `CF_UNICODETEXT`, and consider clearing the clipboard after about 30 s if it still holds the token.

### [S3] mDNS/HTTP bind address chosen from `getaddrinfo(gethostname())`
- Location: src/platform/system/Console.cpp:202-206 and :240-244 (`localAddresses`, `privateLocalAddresses`), consumed at src/platform/scanner/HttpServerLifecycle.cpp:48-49
- Category: correctness
- Failure scenario: `getaddrinfo(hostName)` returns addresses by name resolution, in unspecified order, and includes virtual adapters (Hyper-V/WSL vEthernet 172.x, VirtualBox 192.168.56.1, Docker) and link-local 169.254.x.x (accepted by `privateIpv4`). The listener binds to `availableAddresses.front()`, so on a developer or WSL machine it can bind to an adapter the scanner cannot reach and mDNS then publishes that address; if the host name does not resolve (common on domain machines with odd DNS) the list is empty and the server falls back to `127.0.0.1`, which disables mDNS entirely (`isPrivateIpv4` false). Linux enumerates interfaces with `getifaddrs`, so the lists differ between platforms.
- Evidence: `if (getaddrinfo(hostName, nullptr, &hints, &result) == 0) { ... if (!privateIpv4(ntohl(address->sin_addr.s_addr))) continue;`
- Confidence: medium
- Proposed test: factor the selection into a pure function taking an adapter list (address, metric, flags) and test virtual/link-local ordering.
- Proposed fix (sketch): use `GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_...)`, keep only operational non-loopback non-tunnel adapters, prefer the one with a default gateway/lowest metric, and exclude 169.254/16 unless nothing else exists.

### [S3] Quitting from the tray of an interactive process relaunches the background service
- Location: src/app/shell/AppShell.cpp:184-198 with src/app/shell/AppShell.cpp:254-258 and src/platform/system/BackgroundController.cpp:273-297
- Category: correctness
- Failure scenario: with "Background & startup" enabled, an interactive session owns a tray icon. Choosing "Quit Inventatory" sets `backgroundQuitRequested_`, the loop exits, and the shutdown path then runs `restartAsBackgroundService()` because `!startInBackground_ && settings_.backgroundServiceEnabled`, so Inventatory reappears in the tray seconds later. The user documentation promises Quit, and the only real way out is to disable the setting. In a process started with `--background` the same click really quits, so behaviour differs by how the process was started.
- Evidence: `if (finalSaveSucceeded && !updateInstallerLaunched_ && !startInBackground_ && settings_.backgroundServiceEnabled) { if (!backgroundController_.restartAsBackgroundService()) {`
- Confidence: low-medium (may be intended "Quit foreground, keep service"; label and docs say otherwise)
- Proposed test: unit test of the shutdown decision given `backgroundQuitRequested_`.
- Proposed fix (sketch): remember that shutdown was requested through the tray Quit and skip the relaunch, or relabel the menu item.

### [S3] Console close handler returns immediately and handles only `CTRL_CLOSE_EVENT`
- Location: src/platform/system/BackgroundController.cpp:162-168, :342
- Category: concurrency/lifecycle
- Failure scenario: for `CTRL_CLOSE_EVENT` Windows terminates the process once the handler returns (or after about 5 s). The handler only posts `kHideMessage` and returns TRUE at once, so the intended "closing the window hides to tray" can lose the race against process termination; nothing handles `CTRL_LOGOFF_EVENT`/`CTRL_SHUTDOWN_EVENT`, so a hidden background process is killed without the normal save/shutdown path.
- Evidence: `if (type != CTRL_CLOSE_EVENT) return FALSE; ... PostMessageW(window, kHideMessage, 0, 0); return TRUE;`
- Confidence: low (needs a Windows run; documented Win32 behaviour but console host differences exist)
- Proposed test: manual: close the console window with the service enabled, see whether the process survives.
- Proposed fix (sketch): have the handler wait (bounded, for example 2 s) until the hide message was processed, and handle logoff/shutdown by requesting a graceful quit and waiting for the App loop to finish.

### [S3] `forceStopBackgroundService` terminates any older process with the same image path
- Location: src/platform/system/BackgroundController.cpp:214-246
- Category: correctness
- Failure scenario: the filter is "same image path and created before this process". It does not check `--background` or the session. The interactive mutex is `Local\` (per session), so with two sessions of the same user (or any older stray interactive instance) a launch whose graceful stop timed out can `TerminateProcess` another interactive instance in the middle of an edit/save. This only happens after the 12 s polite phase failed, but the kill is unconditional on process role.
- Evidence: `if (_wcsicmp(image.c_str(), own.c_str()) == 0 && GetProcessTimes(...) && CompareFileTime(&created, &ownCreated) < 0 && TerminateProcess(process, 1) != 0)`
- Confidence: low-medium
- Proposed test: Windows integration test with two fake processes is heavy; at least unit-test the filter if extracted.
- Proposed fix (sketch): also require the process to be in the same session and not hold the interactive mutex (or read its command line / use a job object), and only kill the PID that owns the background controller window (`GetWindowThreadProcessId` of the found window).

### [S3] BLE provisioning: stack-captured state used by a WinRT callback, 25 s wait after a failed write, silent discovery failure
- Location: src/platform/scanner/BleProvisioningService.cpp:282-308, :322-342, :111-143
- Category: concurrency
- Failure scenario (a): the `ValueChanged` lambda captures `statusMutex`, `statusChanged` and the flags by reference. WinRT handler removal does not wait for an in-flight invocation, so the handler running on a thread-pool thread can `notify_one()` on `statusChanged` after `provision()` has already returned and destroyed it (narrow race, UB). Also the removal sits inside a `try/catch(...)` that swallows failure, which would leave the handler alive. Use a `shared_ptr` state captured by value.
- Failure scenario (b): if `WriteValueAsync` failed or threw (`writeThrew` or status != Success) the code still waits the full 25 s for a notification that cannot come before classifying the result.
- Failure scenario (c): `discoveryLoop` ignores `watcher.Status()`/`Stopped`; with the Bluetooth radio off or the watcher aborted the list stays empty and the UI keeps saying "Searching for nearby unconfigured Scan R1 devices" forever; an exception only flips `discovering_` to false without reporting.
- Evidence: `statusChanged.wait_for(lock, chrono::seconds(25), [&] { return terminal; });` and `watcher.Start(); for (;;) { ... if (!discovering_) break; sleep 200ms }`
- Confidence: medium (a: low probability, b/c: high)
- Proposed test: inject a fake characteristic (needs an abstraction); at least a test for the outcome classification function if extracted.
- Proposed fix (sketch): shared state struct; skip the wait when the write failed; subscribe to `watcher.Stopped` and surface `BluetoothError`/"Bluetooth is off" through a `lastError()` accessor.

### [S3] Installer/updater process check fails closed on other users' processes
- Location: installer/Install-Inventatory.ps1:72-93 (`Get-InventatoryProcesses`), same pattern in installer/Uninstall-Inventatory.ps1:11-31
- Category: correctness
- Failure scenario: the CIM query matches `inventatory.exe` by name for all users. For processes of another logged-in user (fast user switching, shared PC) a non-elevated user gets `ExecutablePath = $null`, so the function throws "Unable to verify the executable path for Inventatory process N. Close Inventatory and run the installer again." Installation, in-app update and uninstall are all blocked until the other user logs off, although that process runs from a different install root. In update mode the in-app updater has already exited, so the user is left with the old version and a failed marker.
- Evidence: `if ([string]::IsNullOrWhiteSpace($candidate.ExecutablePath)) { throw "Unable to verify the executable path ..." }`
- Confidence: medium
- Proposed test: extend Test-InventatoryUpdate.ps1 with a mocked `Get-CimInstance` returning a process with null path and a different SessionId.
- Proposed fix (sketch): restrict the filter to the current session (`SessionId -eq (Get-Process -Id $PID).SessionId`) and skip (not throw) entries whose path cannot be read when their owner is another session.

### [S3] Uninstaller leaves Credential Manager secrets and hard-codes `Documents\Inventatory`
- Location: installer/Uninstall-Inventatory.ps1:56-65
- Category: correctness/security hygiene
- Failure scenario: the Scan R1 token and DigiKey secret live in Credential Manager (`Inventatory/...` targets) and are never removed, even when the user answers Y to "delete Documents\Inventatory and local settings". The data path is `$env:USERPROFILE\Documents\Inventatory`, which is wrong when Documents is redirected (OneDrive Known Folder Move, supported by `discoverInventatoryDataPath`) or the data directory was moved in Settings, yet the script prints "Inventatory was removed" and deletes nothing. `.staging`, `.backup.*`, `.failed.*` siblings created by the installer are also never cleaned.
- Evidence: `Remove-Item -LiteralPath (Join-Path $env:USERPROFILE 'Documents\Inventatory') -Recurse -Force -ErrorAction SilentlyContinue`
- Confidence: high
- Proposed test: smoke test in Test-InventatoryUpdate.ps1 with a fake `cmdkey` list.
- Proposed fix (sketch): enumerate `cmdkey /list` for `Inventatory/*` targets and delete them when the user opts in; read the data directory from `settings.conf` (or `[Environment]::GetFolderPath('MyDocuments')`) and remove the install siblings.

### [S3] Desktop shortcut is created by the app even when the installer user declined it
- Location: src/ui/pages/onboarding/OnboardingPage.cpp:255-265 (`finishOnboarding`), installer/Install-Inventatory.ps1:319-321
- Category: correctness
- Failure scenario: the installer asks "Create a desktop shortcut? [y/N]"; the default is no. On first run the onboarding completion calls `createDesktopShortcut()` unconditionally on Windows, which creates (and overwrites) `Desktop\Inventatory.lnk` anyway. The Start-menu shortcut made by the installer carries no AppUserModelID, while the app-created one does (`PKEY_AppUserModel_ID` = Kwiatens.Inventatory, StartupRegistration.cpp:137-146), so the two shortcuts group differently on the taskbar.
- Evidence: `const bool shortcutCreated = createDesktopShortcut(shortcutError);` with no consent check.
- Confidence: high
- Proposed test: unit test `finishOnboarding` with a shortcut-consent setting.
- Proposed fix (sketch): drop the call on Windows (the installer owns shortcuts) or ask in the wizard; set the AUMID in `New-Shortcut` too.

### [S3] Windows error text is ANSI-converted or reduced to `?`, and keeps CR/LF
- Location: src/platform/system/StartupRegistration.cpp:27-32 (`FormatMessageA`), src/label_printer/platform/LabelPrinterWindows.cpp:29-61
- Category: correctness
- Failure scenario: `systemError()` returns `FormatMessageA` output verbatim: ACP-encoded (mojibake in the UTF-8 TUI on non-English Windows) and ending in `".\r\n"`, which is inserted into one-line messages ("Unable to update Windows startup: ...\r\n"), breaking the message row. The printer variant strips CR/LF but replaces every non-ASCII wide character with `?` ("Nie mo?na uzyska? dost?pu") although the file already has `narrowFromWide` (UTF-8) a few lines below.
- Evidence: `result.push_back('?');` / `return count == 0 ? "Windows error " + ... : std::string(message, count);`
- Confidence: high
- Proposed test: Windows-only test with a localized error code.
- Proposed fix (sketch): one shared `windowsErrorText(DWORD)` using `FormatMessageW`, trim trailing whitespace/period, convert with `WideCharToMultiByte(CP_UTF8)`.

### [S3] No retry for transient sharing violations on Windows atomic replace/rename, and diagnostics lose the error code
- Location: src/core/storage/AtomicFile.cpp:199-204, src/label_printer/platform/LabelPrinterPlatform.cpp:238-243, src/core/transfer/InventoryTransferOps.cpp:72-76 and rename/removeAll at :39-60
- Category: persistence
- Failure scenario: `MoveFileExW(REPLACE_EXISTING)` / `filesystem::rename` of a directory fail with `ERROR_ACCESS_DENIED`/`ERROR_SHARING_VIOLATION` when an antivirus scan, the search indexer or OneDrive briefly holds a handle on the destination (the default data folder is under Documents, often synced). Saves of `settings.conf`, `quick_labels.conf`, printer config and the restore directory swap fail on the first attempt with no retry; restore then rolls back and tells the user to try again. `TransferOps::replace` also drops `GetLastError()` ("Unable to atomically replace X") whereas the Linux branch includes `filesystemError.message()`.
- Evidence: `if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = "Unable to atomically replace " + destination.string(); return false; }`
- Confidence: medium-low (environment dependent)
- Proposed test: hook-based test (the `TransferOps` hooks exist) simulating two failures then success.
- Proposed fix (sketch): a small helper retrying 5 times with 50-250 ms backoff on error codes 5/32/33 and appending the Win32 message to the error.

### [S3] MSVC build does not set `/utf-8`, and there is no UTF-8 application manifest
- Location: CMakeLists.txt:33-36 (`add_compile_options(/W4 /permissive-)`), no `.manifest` in src/platform/system
- Category: build/portability
- Failure scenario: 25 source files contain non-ASCII string literals (µ, Ω, ×, box and arrow glyphs, "…" in AppUpdate.cpp:106) and none has a BOM. Without `/utf-8` MSVC decodes sources using the build machine's ACP. On a single-byte ACP the bytes happen to round-trip (CI works), but on a DBCS ACP (932/936/949/950) the literals are re-encoded or cut (C4819) and the UI/labels are garbled. Independent of that, the missing `activeCodePage=UTF-8` manifest is the root cause of the ANSI path findings above.
- Evidence: `if(MSVC) add_compile_options(/W4 /permissive-)` (no `/utf-8`).
- Confidence: medium
- Proposed test: CI job (or local) building under a non-1252 ACP, or a `static_assert` on `u8"…"` literal length.
- Proposed fix (sketch): add `/utf-8` and an embedded manifest with `<activeCodePage>UTF-8</activeCodePage>` (keep the explicit CP_UTF8 conversions).

### [S3] Windows accepts a relative data directory that Linux rejects
- Location: src/app.cpp:65-95 (`#ifndef _WIN32` around the `is_absolute()` check)
- Category: correctness (platform asymmetry)
- Failure scenario: a relative `data_directory` in `settings.conf` (hand edit, older build, or an ACP-mangled path from the dialog finding) is accepted on Windows and resolved against the current directory. The shortcut starts in the install folder while the Run-key launcher starts `inventatory.exe --background` with the inherited CWD (typically `System32`), so the interactive app and the background service open different workspaces and each saves its own database.
- Evidence: `#ifndef _WIN32 if (!settings_.dataDirectory.is_absolute()) { inventoryRecoveryRequired_ = true; ...`
- Confidence: low-medium
- Proposed test: settings load test with `data_directory=Documents\Inventatory` expecting recovery-required on Windows too.
- Proposed fix (sketch): apply the check on both platforms (on Windows also reject rooted-but-not-absolute `\foo`, `C:foo`).

### [S3] Windows HTTP accept loop spins on a persistent `accept` failure
- Location: src/platform/scanner/HttpServer.cpp:87-96 (`acceptLoop`)
- Category: concurrency
- Failure scenario: `accept` returning `INVALID_SOCKET` while `running_` is true goes straight to `continue`. A persistent error (`WSAENOBUFS`, `WSAEMFILE`, adapter loss after sleep/resume) makes the acceptor busy-loop at 100% of a core until the condition clears, with no backoff or logging.
- Evidence: `if (client == kInvalidSocket) { if (running_.load()) continue; break; }`
- Confidence: medium
- Proposed test: not practical; review only.
- Proposed fix (sketch): sleep 20-50 ms on error (except `WSAEINTR`/`WSAECONNRESET`) and surface repeated failures.

## Test gaps
- `environmentValue` / `appSettingsDirectory` with a non-ACP profile path -> Windows-only test using `SetEnvironmentVariableW` and checking the resulting `path`.
- `copyToClipboard` / dialog helpers -> not unit tested; extract the wide/narrow conversion and test it.
- `BackgroundController` (single-instance mutex, `stopBackgroundService`, `forceStopBackgroundService` filter, `restartAsBackgroundService` failure) -> no test references; at least test mutex acquire/second acquire/release and the stop-result mapping with two controllers in one process.
- `launchUpdateInstaller` Windows quoting (`quoteWindowsArgument` with spaces, trailing backslash, quotes) -> no test; expose it as an internal helper and test against the `CommandLineToArgvW` round trip.
- `downloadReleaseAsset` Windows host/URL checks -> only an invalid-host case is tested; add a test that a `github.com` URL with the wrong repo/tag/asset is rejected (Linux does this via `buildReleaseAssetUrl`).
- `MdnsService` Windows registration timeout/cancel path and `HttpServer` double-bind on one port -> Windows-only tests.
- `CredentialStore` on Windows: unavailable vs missing (T7), blob-size overflow (secret > 2560 bytes) returning false, and `workspaceDigest` for equivalent spellings (`c:\x` vs `C:\X`, trailing slash, non-ASCII case).
- `Install-Inventatory.ps1`: checksum-manifest parsing with trailing whitespace/CRLF, null `ExecutablePath`, activation-failure rollback with relaunch of the old exe (only partially covered by `-TestActivationFailure`).
- Uninstall script has no smoke test at all (not in Test-InventatoryUpdate.ps1).

## Quality (S4)

### [S4] Windows update download validates only the host, Linux validates the whole URL
- Location: src/platform/system/UpdateService.cpp:655 (Windows) vs :820-840 (Linux)
- Category: security/quality
- Detail: Windows accepts any `https://github.com/<path>`; Linux requires `buildReleaseAssetUrl(repo, tag, asset) == url`. The only caller builds the URL with `buildReleaseAssetUrl`, and SHA-256 verification follows, so this is defense in depth, but the two branches should share the exact-URL check (and the Windows code sets no redirect restriction, whereas Linux limits redirects to https).
- Proposed fix: move the strict check above the `#ifdef`; set `WINHTTP_OPTION_REDIRECT_POLICY` to `..._DISALLOW_HTTPS_TO_HTTP` explicitly.

### [S4] mDNS host name is widened byte-by-byte (Latin-1) from the ANSI `gethostname`
- Location: src/platform/scanner/MdnsService.cpp:169-171
- Detail: `wideHost.push_back(static_cast<unsigned char>(ch))` mis-decodes non-ASCII computer names (ACP bytes are not Latin-1). Use `GetComputerNameExW(ComputerNameDnsHostname)` and sanitize to a DNS label.

### [S4] Dead declarations and unused code in the Windows console layer
- Location: src/platform/system/Console.h:50-70 (`ConsoleSession`, `consoleSize`, `clearConsole`, `hideCursor`, `showCursor`, `pollKeys`, `buildTerminalCandidateArgs`)
- Detail: none of these has a Windows definition or a caller outside ConsoleLinux.cpp (`pollKeys` is a stub returning `{}`), the Windows `setConsoleTitle`/`ensureTerminalAttached` are trivial. FTXUI owns the console now; remove the dead API to avoid a link error the day somebody calls one on Windows.

### [S4] `controlModifierPressed()` polls global key state
- Location: src/platform/system/Console.cpp:51-58 (used at app/shell/AppShell.cpp:55)
- Detail: `GetAsyncKeyState` reports the physical keyboard regardless of focus, so holding Ctrl in another window turns Backspace into CtrlBackspace (word delete) in the TUI. Prefer deriving it from the console input record (`ReadConsoleInput` modifier state) or from the FTXUI event.

### [S4] Duplicated helpers across the Windows platform files
- Location: `currentExecutablePath()` in BackgroundController.cpp:67, StartupRegistration.cpp:34 and BackgroundLauncher.cpp; `widen`/`narrow` in CredentialStore.cpp:28-42, DigiKeyTransport.cpp:100-124, UpdateService.cpp:50, LabelPrinterWindows.cpp:63-95; WinHTTP session boilerplate (UpdateService x2, DigiKeyTransport) with 20+ hand-written `WinHttpCloseHandle` triples.
- Detail: one small `platform/system/WindowsUtil.h` (UTF-8 conversion with error check, `currentExecutablePath`, RAII `HINTERNET`) would remove the duplicated error paths and prevent handle leaks when a new early return is added. `CredentialStore::widen` also ignores a failed conversion (`count == 0` for invalid UTF-8) and would write an empty blob instead of failing.

### [S4] `Get-ExpectedChecksum` assigns to `$matches`
- Location: installer/Install-Inventatory.ps1:52
- Detail: `$matches` is PowerShell's automatic regex-match variable. It works here only because the assignment happens after the pipeline; rename it (and tolerate trailing whitespace like the C++ parser: `$fields.Count -eq 2` rejects a line with trailing blanks that `parseSha256Checksum` accepts).

### [S4] AtomicFile on Linux never fsyncs the parent directory (Windows uses WRITE_THROUGH)
- Location: src/core/storage/AtomicFile.cpp:205-211
- Detail: after `rename` the directory entry is not synced, so a power loss can still lose the replacement on ext4/XFS; Windows' `MOVEFILE_WRITE_THROUGH` gives the stronger guarantee. Add an `fsync` of the parent directory descriptor.
