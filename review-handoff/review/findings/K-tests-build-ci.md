# Slice K: tests, build, CI

Repo state reviewed: main @ 0ce3761 (includes the UTF-8 input merge and b6c9be6). Read-only; nothing was built or run.

## Coverage

Files read in full: `.github/workflows/release.yml` (the only workflow), `CMakeLists.txt`, `tests/terminal_input_parser_tests.cpp`, `installer/Install-Inventatory.sh`, `installer/Test-InventatoryLinuxInstall.sh`, `packaging/linux/inventatory.desktop`.

Files read in part:
- `tests/inventatory_tests.cpp` (6362 lines). Read in full: lines 1-135, 245-335, 560-660, 1457-1640 (start of `main`), 3830-4135 (HTTP and replay tests), 5036-5066, 5190-5260, 6120-6362. Everything else was searched by function name and comment, not read line by line.
- `installer/Test-InventatoryUpdate.ps1`: structure and assertion names only.
- `installer/Install-Inventatory.ps1`: not read (Windows slice W).
- `docs/linux-support.md` and `docs/public-beta.md`: build, test and CI sections only. `docs/ui-contributor-guide.md`: grepped for build and test instructions.

Production sources read for tracing: `HttpServerConnection.cpp`, `HttpServerProtocol.cpp` (tokensMatch, headerCounter), `InventatoryScanProtocolSecurity.cpp` (transportMac), `AppActionSupport.h`, `CredentialStoreLinux.cpp`, `main.cpp`, `DigiKeyTransportLinux.cpp` (function list only), `LabelPrinterLinux.cpp` (function list only).

External check: FTXUI @182ef70 `cmake/ftxui_set_options.cmake`, fetched with curl, to see which compile options it exports.

Not verified:
- The action SHAs against their version comments (the GitHub API is not reachable from this session).
- Warning-cleanliness of the build (it was not built).
- What `locale -a` contains on the hosted `ubuntu-24.04` runner. The sandbox has only C, C.utf8 and POSIX.

`coverage-raw.tsv` matches function names textually, so it badly over-reports "0 refs" for internal helpers that are tested through a public API (`writeFileAtomically`, `validateInventoryCommitHistory`, `rollbackRestore` and similar are exercised indirectly). The gap map below was re-derived from public entry points.

## T8 confirmation (not re-reported as a finding)

- `inventatory_input_tests` is the only target with plain `<cassert>`. With `CMAKE_BUILD_TYPE=Release` (GCC, `-DNDEBUG`) or the MSVC Release config (`/DNDEBUG`), `assert` is empty and `main()` returns 0 without checking anything.
  - Windows CI is Release-only, so on Windows the test never executes a check.
  - On Linux only the Debug leg effectively tests.
- `inventatory_tests` is not affected. `tests/inventatory_tests.cpp:78-85` does `#undef assert` and defines an always-on assert (print and `std::exit(1)`). It does this after the last `#include` (line 76), so nothing re-includes `<cassert>` over it.
- There are no other test targets. `src/` contains no `assert(` and no `NDEBUG`.
- `CMakeLists.txt` never sets a default `CMAKE_BUILD_TYPE` or `NDEBUG` policy for tests. A single-config build without `-DCMAKE_BUILD_TYPE` is unoptimised and has asserts on. A multi-config build in Release has them off.
- Side note: that target tests FTXUI's own parser at the pinned commit, not the app's key translation (the `ftxui::Event` to `KeyEvent` function in `AppShell.cpp`). It guards the dependency pin, not app behaviour.

## Findings

### [S2] No CI for pull requests or main, and the release workflow cannot run without publishing
- Location: `.github/workflows/release.yml:3-15` (triggers), `:224-315` (publish)
- Category: test-gap
- Failure scenario:
  - A regression merged to main is detected only when someone pushes a `v*` tag. Tests, the Linux build and the Windows build then run on the tagged commit.
  - A red run leaves a pushed tag with no release.
  - The only manual trigger, `workflow_dispatch`, requires an existing tag (`validate`, lines 33-48). `publish` needs `validate`, `build` and `build-linux` and has no dry-run switch, so every green dispatch publishes a public release.
  - There is no way to get a CI result without publishing, so nobody runs CI on feature branches.
  - The docs say "CI can verify Linux compilation and automated core tests" (`docs/linux-support.md:136`). That is true only at release time.
- Evidence:
```yaml
on:
  push:
    tags: ['v*']
  workflow_dispatch:
```
  (There is no `pull_request` and no `push: branches`. `.github/` contains only `workflows/release.yml`.)
- Confidence: high
- Proposed test: not applicable.
- Proposed fix (sketch):
  - Add `.github/workflows/ci.yml` on `pull_request` and `push: branches: [main]`.
    - Jobs: Linux Debug+Release (reuse the build step), Windows Release, and the installer smoke tests.
    - `permissions: contents: read`.
    - `concurrency` group with `cancel-in-progress`.
  - Optionally have `release.yml` call it with `workflow_call` so there is one definition.
  - Add a `dry_run` input to `workflow_dispatch` that skips `publish`.

### [S2] Locale regression tests skip silently, so CI and typical dev boxes never run the locale that broke unit search
- Location: `tests/inventatory_tests.cpp:569-584`, `:622-655`; `.github/workflows/release.yml:170-198`
- Category: test-gap, linux-portability
- Failure scenario:
  - Both locale tests try `pl_PL`, `de_DE` or `fr_FR` UTF-8 locales and `return` with a `cout` line if none is installed. The exit code stays 0.
  - The Linux job installs no `locales` package and runs `locale-gen` for none. The sandbox here has only C, C.utf8 and POSIX, as do Docker, devcontainers and most CI images.
  - So the tests that guard the "dot-decimal parsing under a comma locale" regression are skipped exactly where they matter. A reintroduced `strtod`/`stod` on a Linux desktop with a Polish or German locale would pass CI.
  - Separately, the production binary runs `setlocale(LC_ALL, "")` (`src/main.cpp:109`, Linux only), but the test `main()` never does. The baseline run of every other test happens in the "C" locale, so locale-dependent behaviour (printf/strftime/isalpha families, `tolower` on bytes) is untested in the mode production uses.
  - Windows never calls `setlocale`, so there is no locale coverage there either (acceptable, but the Linux gap is the one that hurt).
- Evidence:
```cpp
if (!commaLocale) {
  setlocale(LC_ALL, previous.c_str());
  cout << "No comma-decimal locale installed; skipping locale regression test\n";
  return;
}
```
- Confidence: high for the skip logic. Medium for the runner contents (the hosted `ubuntu-24.04` image is believed to ship only `C.utf8` and `en_US.utf8`).
- Proposed test:
  - Make skipping a failure when `INVENTATORY_REQUIRE_LOCALE_TESTS=1` is set.
  - Run the whole core test binary once more under `LC_ALL=de_DE.UTF-8` or `pl_PL.UTF-8`, with a startup `setlocale(LC_ALL, "")` mirroring `main.cpp`.
- Proposed fix (sketch):
  - CI step: `sudo apt-get install -y locales && sudo locale-gen de_DE.UTF-8 pl_PL.UTF-8`.
  - Run `LC_ALL=pl_PL.UTF-8 INVENTATORY_REQUIRE_LOCALE_TESTS=1 ctest ...` in addition to the plain run.
  - Add `setlocale(LC_ALL, "")` at the top of the test `main()` on non-Windows, with a UTF-8 or C fallback like `main.cpp`.
  - Widen the locale tests to the other number paths: CSV quantity and price parsing, label text formatting, settings load, scanner JSON numbers.

### [S2] The App orchestration layer is not in the test target, and that is where the S2 defects cluster
- Location: `CMakeLists.txt:243-331` (the `inventatory_tests` source list); `src/app/persistence/AppPersistence.cpp`, `src/app/scanner/AppDeviceActions.cpp`, `src/app/import/AppImportActions.cpp`, `src/app/inventory/AppInventoryEdit.cpp`, `src/app/bom/AppBomEnrichment.cpp`, `src/app/labels/AppQuickLabelActions.cpp`, `src/app/scanner/AppScanEnrichment.cpp`, `src/app/persistence/AppBackupRestore.cpp`
- Category: test-gap
- Failure scenario:
  - `inventatory_tests` compiles only `AppBootstrap.cpp`, `AppSettings*.cpp` and the platform and core files. Roughly 4,500 lines of workflow code (save/retry state, device-event draining, import commit, edit commit, DigiKey enrichment, restore UI flow) are never linked into any test.
  - Triage items T11, T12, T14, T15, T20 and T23 are all in this layer. They are state-machine bugs (a pending-save flag, a whole-store overwrite, a cached failure, a spinning queue). Each could be caught by a unit test that does not need a terminal.
  - `hasPendingPersistence()` (`AppPersistence.cpp:349`) has no test at all. AGENTS.md calls the in-memory retry path an invariant.
- Evidence: `grep -c` of `hasPendingPersistence|persistedStore_` in `tests/inventatory_tests.cpp` returns 0. `src/app/` is 7,954 lines, of which the test target compiles AppBootstrap and AppSettings.
- Confidence: high
- Proposed test:
  - For T14: save fails, then a device event is processed, then the commit's change counts are validated against its snapshots.
  - For T15/T20: begin an edit or import, apply a device event, commit, and assert the scanner quantity survives.
  - For T23: a failing DigiKey lookup must not be cached as a value.
- Proposed fix (sketch):
  - Extract narrow, UI-free units that `App` calls: `processDeviceSyncEvents` core, "commit draft against base" with a conflict check, an enrichment cache.
  - Put them in a small static library shared by `inventatory` and `inventatory_tests`.
  - AGENTS.md already allows "extract only a narrow, behavior-preserving unit".

### [S3] `/utf-8` reaches the app only by accident, and there is no UTF-8 manifest (root cause behind T13/T24/T25)
- Location: `CMakeLists.txt:33-37`, `:219-225`; `src/platform/system/Inventatory.rc`, `BackgroundLauncher.rc`
- Category: linux-portability (Windows/Linux divergence), correctness
- Failure scenario:
  - The project sets no `/utf-8`, no `/source-charset`/`/execution-charset`, and no application manifest with `<activeCodePage>UTF-8</activeCodePage>`. (`grep` for `utf-8|manifest|activeCodePage` in `CMakeLists.txt` and `src` finds only `charset=utf-8` HTTP headers and `CP_UTF8` conversions.)
  - `/utf-8` currently applies only because FTXUI exports it as a PUBLIC compile option: `ftxui_set_options.cmake` has `target_compile_options(${library} PUBLIC "/utf-8")`, inherited via `ftxui::screen`.
    - Many sources contain non-ASCII literals (`·`, `●`, `▌`, `→`, `↶`, `••••`, and `u8"▀"` in `OnboardingPage.cpp:103`). Without `/utf-8`, MSVC reads them as the ANSI code page, and `u8"…"` literals would be double-encoded.
    - A dependency bump or a switch away from FTXUI's option silently corrupts the UI glyphs.
    - `inventatory_background`, which does not link FTXUI, never gets the flag.
  - The "A" Win32 entry points keep using the process ACP, which matches the T24 (`_dupenv_s`) and T25 (`GetOpenFileNameA`, `SHBrowseForFolderA`) path bugs. The triage note says "UTF-8 manifest + `/utf-8`" fixes the cluster.
- Evidence: `CMakeLists.txt:33-37` has only `add_compile_options(/W4 /permissive-)`. Source lines with non-ASCII literals include `HistoryPageRender.cpp:113`, `OnboardingPage.cpp:103`, `SettingsPage.cpp:508`.
- Confidence: high for the missing flag and manifest. High that the FTXUI option is what saves it today.
- Proposed test:
  - Add a build-time static check, for example a test TU compiled with the project flags asserting `sizeof(u8"▀") == 4`.
  - Run a Windows CI step that launches `inventatory.exe` against a `LOCALAPPDATA` containing non-ASCII characters (T24).
- Proposed fix (sketch):
  - Add `if(MSVC) add_compile_options(/utf-8) endif()` (project-wide, including `inventatory_background`).
  - Add a manifest (`.manifest` or an `.rc` `RT_MANIFEST` entry) with `activeCodePage` UTF-8 for Windows 10 1903+ and `longPathAware`.
  - Then reduce the ANSI-API call sites.

### [S3] No sanitizer, no Clang build, no `-Werror`, and Windows is Release-only
- Location: `CMakeLists.txt:31-37`; `.github/workflows/release.yml:60-73`, `:177-198`
- Category: test-gap, quality
- Failure scenario:
  - The code base has a multi-threaded HTTP service with epoch and mutex invariants, fork-based process tests, SQLite handles and manual socket code. There is no ASan/UBSan job and no TSan job.
  - CMake has no `-fsanitize` option. The CI test legs are GCC `-O0` Debug and GCC `-O3` Release, with default libstdc++ (no `_GLIBCXX_ASSERTIONS`).
  - Warnings are `-Wall -Wextra -Wpedantic` and MSVC `/W4`, never errors. A new warning (the usual signal for a narrowing, sign-compare or lifetime bug) cannot fail a build.
    - The flags are global, so they also hit sqlite3.c.
  - Windows builds only `--config Release`, so a Debug-only MSVC issue (iterator debugging, `_ITERATOR_DEBUG_LEVEL` asserts) is invisible. Combined with T8, Windows has no input-parser test at all.
- Evidence: `release.yml:177-198` is the only test step. No `-fsanitize`, `-Werror` or `/WX` anywhere (`grep`).
- Confidence: high
- Proposed test: not applicable.
- Proposed fix (sketch):
  - Add `option(INVENTATORY_SANITIZE "address;undefined|thread")` adding `-fsanitize=... -fno-omit-frame-pointer` to the test and app targets on GCC/Clang.
  - CI legs: Linux Debug+ASan/UBSan, Linux TSan for the scanner and HTTP tests (the fork-based background tests need care), Clang Release.
  - Add `-D_GLIBCXX_ASSERTIONS` to Debug.
  - Make warnings errors for first-party targets only, in a CI-only configure flag (`-DINVENTATORY_WERROR=ON`), keeping sqlite and FTXUI out of it.
  - Add a Windows Debug build+test leg.

### [S3] Core tests need a live, unlocked Secret Service. Nothing documents it, and failure aborts the whole suite
- Location: `tests/inventatory_tests.cpp:1579-1640`; `.github/workflows/release.yml:170-198`; `docs/linux-support.md:20-28`, `AGENTS.md` (build and test section)
- Category: test-gap, quality
- Failure scenario:
  - A contributor on a headless box, container, SSH session or WSL follows `ctest --test-dir build-linux-debug --output-on-failure` (AGENTS.md and `linux-support.md`). `CredentialStore::write` fails (no D-Bus Secret Service), `assert(wrote)` calls `exit(1)`, and every later test (about 5,000 lines) is skipped.
    - CI works around this with `dbus-run-session` plus an unlocked `gnome-keyring-daemon` (`release.yml:194-198`). That recipe appears in no doc (`grep gnome-keyring docs README.md AGENTS.md` returns nothing).
  - On a desktop, the tests write to and delete from the developer's real login keyring. The second block asserts mid-way (`assert(CredentialStore::writeForWorkspace(...)`), so a failure leaves test credentials behind in the keyring.
- Evidence:
```cpp
const bool wrote = CredentialStore::write(key, "temporary-secret-value");
...
assert(wrote);
```
  (The credential tests are in the main sequence at lines 1579-1640, early in `main()`.)
- Confidence: high
- Proposed test:
  - Run a `CredentialStore` contract test only when a store is available (probe once), printing a skip that is visible in ctest via exit code 77 and `SKIP_RETURN_CODE`.
  - Give `CredentialStore` an in-memory backend for the rest of the tests.
- Proposed fix (sketch): document the `dbus-run-session` recipe in `docs/linux-support.md`. Make the credential tests independent of ambient state (private `XDG_RUNTIME_DIR`/`DBUS_SESSION_BUS_ADDRESS`, or an injectable store), and clean up in a scope guard so asserts cannot leave entries.

### [S3] The Linux installer test uses a synthetic tarball, never the real package. `--update`, the riskiest path, has no test. The Windows update test is not in CI.
- Location: `installer/Test-InventatoryLinuxInstall.sh:11-20`; `.github/workflows/release.yml:199-216`; `installer/Install-Inventatory.sh:121-141`; `docs/public-beta.md:64-66`
- Category: test-gap
- Failure scenario:
  - The smoke test builds `Inventatory-linux-x64.tar.gz` containing only `inventatory`. The real archive (`release.yml:210-211`) adds `LICENSE`, `LICENSE.md`, `README.md`, `THIRD_PARTY_NOTICES.md` and `docs/linux-support.md`. `validate_archive` (`Install-Inventatory.sh:51-73`) rejects any member not on its allowlist, and the real file list is checked by it only when a user installs.
  - A change to the `tar` command, the file list or GNU-tar listing format breaks every Linux install and update with "unexpected path". CI stays green, because the test step runs before the package step and never runs the installer on the real tarball.
  - The test covers a fresh install only. `--update` (target discovery via `/proc/$pid/exe`, waiting for the old process, the staging directory next to the exe, rollback on marker failure) is untested on Linux. This is the area of triage T9.
  - The Windows equivalent, `Test-InventatoryUpdate.ps1` (success, wait, staging failure, rollback cases), is never invoked by any workflow. It is only mentioned as a manual maintainer step.
  - There are no negative cases for the Linux installer either: tampered archive, missing or duplicate checksum entry, a symlink or `../` member.
- Evidence: `release.yml:199-201` runs `bash installer/Test-InventatoryLinuxInstall.sh`. The package step follows and does no installer run. No workflow line references `Test-InventatoryUpdate.ps1`.
- Confidence: high
- Proposed test:
  - After packaging, run the installer against `dist/Inventatory-linux-x64.tar.gz` and `dist/Install-Inventatory.sh` with a temp `HOME` (the CI environment is a clean install).
  - Add tamper cases: flip a byte in the archive, edit the checksum file, add a `../x` member, add a symlink member.
  - Add a `--update` case using a stub parent process with a known `/proc` exe.
  - Run `Test-InventatoryUpdate.ps1` in the Windows job.

### [S3] The built binaries are never launched in CI
- Location: `.github/workflows/release.yml:60-73`, `:177-198`
- Category: test-gap
- Failure scenario: nothing runs `inventatory --version` on either platform's release binary. A missing runtime library, a link problem or a startup crash in the shipped artifact is found by users. `--version` is handled before any terminal or locale setup (`src/main.cpp:92-99`), so it is a cheap smoke check. It would also verify that the tag-derived `INVENTATORY_VERSION` made it into the binary (the packaging step trusts that).
- Evidence: `main.cpp:92-99` prints `Inventatory_VERSION_STRING`. The workflow has no such step.
- Confidence: high
- Proposed test: `test "$(./build-linux-release/inventatory --version)" = "$version"`, and the same for `build\Release\inventatory.exe`.
- Proposed fix (sketch): add the step before packaging. Optionally also `ldd` the Linux binary and fail on "not found".

### [S3] The scanner HTTP negative tests omit most of the cases the AGENTS.md security checklist requires
- Location: `tests/inventatory_tests.cpp:3830-4135`; `src/platform/scanner/HttpServerConnection.cpp:36-107`
- Category: test-gap, security
- Failure scenario: the server tests cover tampered body, duplicate Content-Length, exact replay, rotation, persistence of replay state, a failing durable callback, marker-write failure and corrupt replay state. Not covered:
  - wrong method (GET or PUT on `/api/v1/device/sync`) and unknown paths (the 404 branches)
  - a request signed for another path
  - an `X-Inventatory-Device` that differs from the paired device (the 400 "Device identity does not match the pairing")
  - a body `deviceId` that differs from the header
  - a *stale lower* counter (for example 41 after 42; only the exact repeat is tested)
  - counter `0`, a non-numeric counter, or one that overflows `uint64`
  - a missing or wrong `X-Inventatory-Protocol` (also the 426 branch)
  - a MAC with the wrong length, uppercase hex, or non-hex characters
  - `Transfer-Encoding: chunked` (the 400)
  - a body over `kMaxHttpBodyBytes` (the 413)
  - the empty-token case. `transportMac` returns `""` for an invalid token and `tokensMatch` rejects an empty expected MAC. Neither is asserted: `deviceRequestMac("")` is not tested, nor is a server started with an empty or non-hex token returning 401 for everything.
- Evidence: apart from the two MAC known-vector asserts (`:3765`, `:3768`), the only direct `401` assertions are for a modified firmware string and an old token after rotation. `grep` for `GET /`, `404`, `405` and `426` in the test file returns nothing.
- Confidence: high
- Proposed test: one table-driven case that builds the signed request, mutates one field per row, and asserts the status and that `syncCalls` did not increase.
- Proposed fix (sketch): none needed in production code (each branch is correct on reading). Add the tests.

### [S3] Scanner credential resolution (`resolveWorkspaceScannerCredential`) is untested
- Location: `src/app/common/AppActionSupport.h:43-59`
- Category: test-gap, security
- Failure scenario:
  - `resolveWorkspaceScannerCredential` decides between `Loaded`, `RequiresPairing` and `FreshCredential`. `FreshCredential` makes the caller generate a new token. Related to T7 (`CredentialStore::read` returning "missing" when the keyring is unavailable): with `config.setupComplete == false` and an empty `deviceId`, an unavailable keyring becomes `FreshCredential`, which could replace a real token. No test exercises any branch, including `validScannerToken` (64 hex chars) with uppercase, short or non-hex input.
  - The header is not included by any test (`validScannerToken`, `resolveInventoryDatabasePath` and `upsertParameter` all show 0 references).
- Evidence: the function body at `AppActionSupport.h:48-59`.
- Confidence: high that it is untested. Medium that the T7 interaction is exploitable.
- Proposed test: a fake `CredentialStore` seam (or the real store with a private service) and a table of (stored token, config) to status.
- Proposed fix (sketch): add the test alongside the T7 fix. Once `CredentialStore::read` can distinguish "missing" from "unavailable", resolve to `RequiresPairing` or an error on "unavailable".

### [S3] Updater download, launch and relaunch have no test seam
- Location: `src/platform/system/UpdateService.h:33-55`; `tests/inventatory_tests.cpp:5148-5200`
- Category: test-gap
- Failure scenario:
  - `parseReleaseMetadata`, `buildReleaseAssetUrl`, `parseSha256Checksum`, version comparison and hash verification are well covered. `downloadReleaseAsset` is tested only for the URL allowlist rejection (`:5157`).
  - The transfer itself (curl or WinHTTP) cannot be tested: size limit, timeout, redirect to a non-GitHub host, truncated body, partial file left behind, progress callback aborting.
    - T4 (a 15 s total transfer timeout for up to 512 MB) escaped for exactly this reason.
  - `launchUpdateInstaller`, `relaunchAfterUpdate` (Linux `exec` handoff), `checkLatestRelease` and `checkLatestScanFirmwareRelease` have 0 test references.
- Evidence: `grep -c` of `launchUpdateInstaller|relaunchAfterUpdate|checkLatestRelease` in the tests returns 0.
- Confidence: high
- Proposed test: inject the transport (function pointer or interface) so a fake can return chunks, stall, or return a mismatched hash. Assert that the destination is removed on every failure path, that the size cap holds, and that a stall past the idle limit (not the total time) fails.
- Proposed fix (sketch): a `HttpGetStream` interface used by `downloadReleaseAsset`, with the curl and WinHTTP implementations behind it. Fix T4 at the same time (idle timeout instead of total).

### [S3] The Linux DigiKey transport (b6c9be6) and the DigiKey response parsers have no tests
- Location: `src/platform/digikey/DigiKeyTransportLinux.cpp` (retry logic, `isSafeHeaderValue`, `appendAuthorizationHeader`, `encodeComponent`, `receiveBody` size limits); `DigiKeyProductDetails.cpp`; `DigiKeyProduct.cpp`
- Category: test-gap
- Failure scenario:
  - The only DigiKey tests are `validateDigiKeyJsonPayload`, config validation and `isFiniteDecimal`. `parseProductDetails`, `resolveSearchResult`, `mergeDigiKeyMetadata`, `requestToken`, `ensureAccessToken` and `requestKeywordSearch` have no coverage.
  - The owner's Linux transport, just committed, is the only code path on Linux that sends the client secret and bearer token. There is no test that CR/LF in a header value is rejected (`isSafeHeaderValue`: 0 refs), that URL components are encoded, that the response size is capped, or that the retry policy stops. Nothing asserts that the secret does not reach an error string.
  - T23 (a DigiKey failure cached as "-") also sits in the untested enrichment path.
- Evidence: coverage-raw.tsv lists `isSafeHeaderValue`, `appendAuthorizationHeader`, `encodeComponent`, `requestHttp`, `parseProductDetails` and `resolveSearchResult` with 0 test refs. `grep -on 'digikey_detail::[A-Za-z]*'` in the tests gives only `isFiniteDecimal`.
- Confidence: high
- Proposed test:
  - Fixture JSON for keyword search and product details (mojibake parameter names like `25Â°C`, missing fields, huge arrays).
  - Header-injection rows for `isSafeHeaderValue`.
  - A local loopback server for the curl transport: 429 with Retry-After, a 5xx then 200, an oversized body, a redirect.
- Proposed fix (sketch): expose `requestHttpOnce` behind an injectable function so the retry loop can be tested without curl.

### [S3] Linux label-printer CUPS parsing is compiled into the test binary but never exercised
- Location: `src/label_printer/platform/LabelPrinterLinux.cpp:22-67`, `:131-182`; `tests/inventatory_tests.cpp:371-380` (only a `MockPrinterBackend`)
- Category: test-gap
- Failure scenario:
  - `${INVENTATORY_PRINTER_SOURCE}` is linked into `inventatory_tests`, but all printing tests use the mock backend. `enumeratePrinters`, `probePrinter`, `runCommand` and `validQueueName` (queue-name validation before `execvp`) have no direct test.
  - T21 (a disabled queue reported as available) and T22 (`lpstat` parsed in the user's locale) are the kind of bug fixture-based tests catch. There is no seam, so no fixtures can be fed.
- Evidence: `grep -n "lpstat\|probePrinter\|enumeratePrinters\|CUPS"` in the test file finds only the mock's overrides.
- Confidence: high
- Proposed test: parse functions taking the command output as a string, with fixture outputs for: an enabled queue, "disabled since …", "idle", a de_DE translated output, a queue name with a space or a leading `-`.
- Proposed fix (sketch): split `runCommand` from parsing, so parsing is a pure function in the private header. Run `lpstat` with `LC_ALL=C` (T22).

### [S3] Release publication is not robust to partial failure, and the jobs have no timeout
- Location: `.github/workflows/release.yml:224-315` and all jobs; `CMakeLists.txt:342-345`
- Category: quality
- Failure scenario:
  - `gh release create $tag ... assets` creates a public, non-draft release and uploads in the same call. If an upload fails midway, a public release with missing assets exists. The re-run guard then refuses (`:310-313` "Release $tag already exists; published assets are immutable") until someone deletes it by hand.
    - The updater reads `releases/latest` metadata. A half-published release can be picked up by clients that then fail asset lookup.
  - No job has `timeout-minutes` (default 360 minutes), no ctest `--timeout` or `TIMEOUT` test property is set, and `add_test` has no timeout. The core test includes a fork/exec sleeper and slow-client socket tests. A hang costs six hours of runner time.
  - There is no `concurrency` group. A tag push plus a manual dispatch for the same tag race the "does it exist" check and the create (non-atomic).
  - `actions/upload-artifact` with `path: dist/*` also uploads the unpacked `dist/Inventatory/` tree next to the archive on both OSes (harmless, but duplicate and large).
- Evidence: `:310-314` check-then-create. `grep timeout-minutes .github` returns nothing.
- Confidence: high
- Proposed test: not applicable.
- Proposed fix (sketch):
  - `gh release create --draft ...`, then `gh release edit --draft=false` only after a `gh release view --json assets` verification.
  - Add `timeout-minutes: 45` per job and `set_tests_properties(... TIMEOUT 300)`.
  - Add `concurrency: group: release-${{ inputs.tag || github.ref_name }}`.

### [S3] No build provenance and no signing. Checksums travel with the assets they protect.
- Location: `.github/workflows/release.yml:139-151`, `:212-216`; `installer/Install-Inventatory.sh:38-47`
- Category: security
- Failure scenario:
  - The updater and both installers verify SHA-256 against `SHA256SUMS*.txt`, but those files are uploaded to the same release, so anyone able to replace one release asset can replace the other.
  - The workflow does not use `actions/attest-build-provenance` (it grants no `id-token` or `attestations: write`), and the Windows executables are not Authenticode-signed.
  - `AGENTS.md` calls updater integrity a first-class concern, and the updater applies archives automatically.
  - The workflow is otherwise well hardened: top-level `contents: read`, `contents: write` only in `publish`, `persist-credentials: false`, actions pinned by SHA, inputs passed through `env:`, `github.token` only in the `gh` step, and the tag resolved to a SHA before any build. These deserve to stay.
- Evidence: no `attestations`, `id-token` or signing step in the workflow.
- Confidence: high (facts), medium (priority).
- Proposed test: not applicable.
- Proposed fix (sketch): add `actions/attest-build-provenance` for the zip, the tarball and the checksum files (job-level `id-token: write`, `attestations: write` on the build jobs only), and verify with `gh attestation verify` in the docs. Longer term, sign the Windows executables.

### [S3] Test fixtures use fixed, predictable `/tmp` names, share them across targets, and are not cleaned up on failure
- Location: `tests/inventatory_tests.cpp:796`, `:989-990`, `:1271`, `:3838`, `:5464-5471`, `:6127` and about 45 more `temp_directory_path() / "inventatory-..."` sites
- Category: quality, test-gap
- Failure scenario:
  - Names are constant (`inventatory-http-test`, `inventatory-transfer-bundle`, `inventatory-transfer-restore-target`, ...). Debug and Release ctest runs started concurrently (an IDE, `ctest -j`, two terminals) clobber each other. The tests `remove_all` the path at the start, so a second run deletes the first run's live fixtures mid-test.
  - On a shared multi-user machine another user's leftover directory with the same name makes the test fail on permissions. A predictable name in a world-writable `/tmp` is also the classic symlink-attack setup.
  - The `createdir` test uses `time(nullptr)` (1-second resolution) as the suffix (`:6127`). Other tests use `getpid()` (`:4746`, `:6154`, `:6176`, `:6290`), so the convention is inconsistent.
  - Because the assert macro calls `std::exit(1)`, every failure leaves its directories behind, and the next run starts by deleting them without anyone seeing the evidence.
  - Port collisions are not an issue: `LocalHttpServer::start` scans 20 ports from the preferred one and the tests use `server.port()`.
  - Several tests bind the first private LAN address found (`privateLocalAddresses()`), so they depend on the host's network configuration (falling back to loopback when none exists).
- Evidence: `filesystem::temp_directory_path() / "inventatory-transfer-bundle"`. `remove_all` runs at the start of the same block (`:5473-5480`).
- Confidence: high
- Proposed test: not applicable.
- Proposed fix (sketch): one `TempDir` RAII helper that creates `mkdtemp`/`create_directory` under `temp_directory_path()` with a unique name (PID plus counter), and removes it in the destructor. Replace the `exit(1)` assert with one that throws, so destructors run (or use a `atexit` cleanup list).

### [S3] AGENTS.md's Windows build command omits `inventatory_input_tests`, so the documented `ctest` fails
- Location: `AGENTS.md:91`; compare `docs/public-beta.md:48` and `release.yml:72`
- Category: quality (docs)
- Failure scenario:
  - AGENTS.md says `cmake --build build --config Release --target inventatory inventatory_background inventatory_tests -- /m:1` followed by `ctest ...`. `inventatory_input_tests` is not in that list, so ctest reports `inventatory_input` as "Not Run" (executable not found), and the documented sequence ends red. The CI command and `public-beta.md` include all four targets.
  - Agents and contributors following AGENTS.md literally "inspect failures" that are caused by the doc.
- Evidence: AGENTS.md line 91 versus `release.yml:72`.
- Confidence: high
- Proposed test: not applicable.
- Proposed fix (sketch): add `inventatory_input_tests` to the AGENTS.md command, or build the default `all` target, or make the test target a dependency of the test (`add_dependencies`).

## Test gaps

Ranked coverage-gap map (risk first). "Tested" means the public entry points have direct assertions.

1. Persistence and restore
   - Tested well: commit history, schema validation, backup create/validate, restore with injected failures (copy/rename/remove/saveSettings hooks), journal recovery after a crash at several points, settings atomic write, quick-label config.
   - Gaps:
     - Restore over a destination containing files outside the 5-file allowlist, plus an assertion that the pre-restore backup contains them (T10, S1 data loss, currently unguarded).
     - `replaceFile` hook (declared in `InventoryTransferTestHooks`) is never set by any test.
     - The App-level restore flow (`AppBackupRestore.cpp`: stopping the scanner and mDNS, token rotation, replay-state clearing, device identity removal) is not linked into tests.
     - After a failed save: retry and `hasPendingPersistence()` (S2 finding above).
     - A concurrent writer plus the online-backup WAL case is covered (`:5871`); a read-only or full disk is not.
2. Scan R1 protocol and HTTP service
   - Tested well: MAC known vectors, replay and rotation, durable-callback failure, marker failure, slow-client isolation, workspace-scoped counters, sync-request parsing (14 refs).
   - Gaps:
     - the negative matrix in the S3 finding above
     - `parseDebugReportJson`/`parseStatusReportJson`: 0 refs
     - `generateInventatoryScanToken`: 1 ref (no length/charset/entropy check)
     - restart persistence is covered once; interrupted writes of `replay.state` (temp file left behind, truncated file) are not
     - `resolveWorkspaceScannerCredential` (S3 finding above)
     - `LocalHttpServer` on a machine with no private address (falls back to 127.0.0.1) is covered only incidentally
     - `MdnsService.cpp` and `BleProvisioningService*.cpp` are not in the test target at all (T2 `avahi-publish-service --interface` escaped as a result). `waitForRegistrationCompletion` and the avahi argv builder could be tested against a fake `avahi-publish-service` script on PATH, the same pattern the systemctl shim test already uses.
3. Import parsers
   - Tested: delimiter sniffing, BOM strip, format detection, oversize limits at field/row/file level, KiCad lenient quote recovery, malformed quantity, DigiKey order CSV.
   - Gaps:
     - CRLF input, a quoted newline inside a field, an unterminated quote at EOF
     - non-UTF-8 bytes (a Windows-1250 export from a Polish Excel) and UTF-16 with BOM, with the expected error/replacement behaviour
     - embedded NUL
     - `findColumn`/`anyHeaderMatches` alias collisions
     - the import commit itself (`AppImportActions.cpp`, T20) is untested (S2 finding above)
4. Settings
   - Tested: load/save round trip, unsupported/legacy/invalid values, atomic auxiliary files, quick-label revision limits.
   - Gaps:
     - `appSettingsPath()`/`appSettingsDirectory()` (0 refs), including `XDG_CONFIG_HOME` empty, relative or unset with `HOME` unset
     - a save that fails after the credential write and the rollback in `SettingsPageSave.cpp` (T7)
     - concurrent read of settings by the background callback (the mutex discipline in AGENTS.md has no test)
5. Updater: see the S3 finding. Also `isUpdateCheckDue` is tested but `checkLatestRelease` is not; the 512 MB cap and the redirect policy have no tests.
6. Label printing: see the S3 finding. ZPL generation (`LabelPrinterZpl.h`: `writeText`, `writeGraphic` and similar) is tested only indirectly. `sanitiseZplFragment` and `sanitizeLabelText` are the injection boundary, with no direct `^`, `~`, `\r\n` or oversized-input test (the 3 test references found are incidental).
7. Platform/UI shell
   - `Console.h`: `openUrl`, `copyToClipboard`, `openCsvFileDialog`, `saveFileDialog`, `openFolderDialog`, `localAddresses`, `requestTerminalAttention` (0-1 refs each). These shell out on Linux (xdg-open, wl-copy, zenity or kdialog) with user-influenced strings, so an argument-injection test with a PATH shim would fit the existing pattern.
   - `BackgroundController::restartAsBackgroundService`, `waitForBackgroundServiceToStop`, `interactiveInstanceRunning`: 0 refs. Takeover, stubborn-service kill, a stale PID and SIGHUP are covered on Linux; the `(deleted)` exe suffix (T9) and disabling the service while the TUI runs (T3) are not.
   - The Windows `BackgroundController.cpp`/`Console.cpp` have no tests (all of that block is `#ifndef _WIN32`).
   - The `ftxui::Event` to `KeyEvent` translation in `AppShell.cpp` is untested. The T1 fix tests `appendKeyText`/`eraseLastCharacter` but not the producer.

## Quality (S4)

### [S4] One 6,362-line file, a single ctest entry, and a 4,900-line `main()`
- Location: `tests/inventatory_tests.cpp:1457-6362`; `CMakeLists.txt:343`
- Category: quality
- Failure scenario: ctest sees one test, `inventatory_core`. The first failing `assert` calls `exit(1)`, so a single regression hides every later one, there is no per-area name or label, and there is no way to run "only restore" while iterating. Environment mutation (`setenv`, signal handlers, the working PATH) is interleaved with unrelated blocks in one process. Only eight named `testXxx()` functions exist.
- Evidence: `grep -n "^int main"` gives line 1457; the file ends at 6362.
- Confidence: high
- Proposed fix (sketch): AGENTS.md wants to keep one test program, so keep it, but add a tiny registry (`TEST(name) { ... }` plus `argv[1]` filter) and register one ctest entry per group via `--filter`, or split blocks into `void testX()` functions called from `main`. The assert should record failures and keep going, or throw.

### [S4] The whole source list is duplicated between the app and test targets, and the tests are built even when `BUILD_TESTING=OFF`
- Location: `CMakeLists.txt:92-215`, `:243-331`, `:342-345`
- Category: quality, perf
- Failure scenario:
  - About 90 files are compiled twice per configuration (so four times per CI run). Adding a file requires editing two lists, and a missed entry produces a link error only for the test target.
  - `add_executable(inventatory_tests ...)` is unconditional; `BUILD_TESTING` only gates `add_test`. `-DBUILD_TESTING=OFF` still compiles the tests, and `cmake --build` with no target builds them for ordinary users.
- Evidence: `if(BUILD_TESTING)` covers lines 342-345 only.
- Confidence: high
- Proposed fix (sketch): an `inventatory_core` static library holding the shared sources (which also helps the S2 App-layer finding), and `if(BUILD_TESTING)` around the test executables.

### [S4] Weak assertions
- Location: `tests/inventatory_tests.cpp:3460`, `:615`
- Category: quality
- Failure scenario:
  - `assert(utf8.text.find(u8"Ω") == 0 || utf8.text == "...")` passes if `fitFont0Text` degrades to `"..."`, so it cannot detect a regression that drops all content. The expected result for 8 Omegas in 20 dots should be pinned.
  - `findClosestPhysicalValues(items, "100nF").size() >= 2` does not check which items.
- Evidence: the lines above.
- Confidence: medium
- Proposed fix (sketch): pin exact outputs.

### [S4] No `.gitattributes`; CRLF is worked around inside a test
- Location: repository root; `installer/Test-InventatoryLinuxInstall.sh:19`
- Category: quality
- Failure scenario: the Linux installer test pipes the installer through `sed 's/\r$//'`, which shows CRLF checkouts have been an issue. The release job packages `installer/Install-Inventatory.sh` as checked out. On Linux that is LF, but any Windows checkout with `core.autocrlf=true`, or a maintainer's local `dist` build, ships a script whose shebang or `sh` parsing breaks, and its hash then mismatches the tested one. The `.desktop` file and the `.cmd`/`.ps1` installers are also line-ending sensitive.
- Evidence: `git ls-files --eol installer` shows `i/lf w/lf attr/` (no attributes set).
- Confidence: medium
- Proposed fix (sketch): `.gitattributes` with `*.sh text eol=lf`, `*.desktop text eol=lf`, `*.cmd text eol=crlf`, `*.ps1 text eol=crlf`, `*.cpp *.h *.md text eol=lf`.

### [S4] FetchContent: single-host SQLite download with no mirror or cache, fetched twice per CI run
- Location: `CMakeLists.txt:70-89`; `.github/workflows/release.yml:184-193`
- Category: quality
- Failure scenario: the pins are good (SQLite by SHA3-256 `URL_HASH`, FTXUI by commit SHA, so no change needed on integrity). However, `https://www.sqlite.org/...` is the only source, so a sqlite.org outage or rate limit fails every CI job (this sandbox cannot reach it either). Each Linux build directory (debug, release) re-downloads and re-clones, with no `actions/cache` for `_deps` and no shared `FETCHCONTENT_BASE_DIR`. There is no `FETCHCONTENT_SOURCE_DIR_*` hint in the docs for offline or distro builds.
- Evidence: `CMakeLists.txt:72-73`.
- Confidence: high
- Proposed fix (sketch): add a second `URL` (for example a GitHub-hosted mirror of the same archive, with the same hash), share `-DFETCHCONTENT_BASE_DIR` between the two Linux configurations, and cache it.

### [S4] Two packaging paths with different contents, and `cmake --install` is never run in CI
- Location: `CMakeLists.txt:347-357`; `.github/workflows/release.yml:201-216`; `docs/public-beta.md:60-62`
- Category: quality
- Failure scenario:
  - The documented source install (`cmake --install build-linux-release --prefix "$HOME/.local"`) installs the binary, docs, a `.desktop` file and eight icon sizes. The release tarball contains only the binary plus docs and is laid out by hand, and the app writes its own launcher at runtime. A missing icon or a wrong `.desktop` would fail only at `install` time on a user machine.
  - There are no `install()` rules for Windows. `release.yml` hand-copies files with an explicit allowlist, so that path is covered by its own checks.
  - The installed `.desktop` has `Terminal=false` and `Exec=inventatory`. This relies on the app re-launching itself in a terminal (`ConsoleLinux.cpp:249-333`), which is fine but untested.
- Evidence: the two lists above.
- Confidence: medium
- Proposed fix (sketch): add `cmake --install build-linux-release --prefix "$PWD/stage"` plus a file-list check (and `desktop-file-validate`) to the Linux job, and build the tarball from that staging directory so the two paths cannot drift.

### [S4] The generated rack-symbol data has a `--check` mode that CI never runs
- Location: `tools/rack_symbols/generate.py:166-170`; `src/label_printer/symbols/RackSymbolsData.generated.cpp` (65 KB)
- Category: quality
- Failure scenario: the file is checked in and "do not edit". If someone edits the SVG or `symbols.json` without regenerating (or edits the generated file by hand), nothing fails. AGENTS.md asks that generated release inputs be verified locally. The generator needs Pillow and a rasteriser, so the check may need a dedicated job.
- Evidence: `if "--check" in sys.argv:` exists. No workflow references it.
- Confidence: medium
- Proposed fix (sketch): add an optional CI job running `python3 tools/rack_symbols/generate.py --check` (pin the renderer for determinism), or at least a `git diff --exit-code` after regenerating.

## CI and CMake checklist

| Question | Answer |
|---|---|
| Linux CI job exists | Yes, `build-linux` (ubuntu-24.04, tag-triggered only; see S2 finding 1) |
| Debug + Release matrix | Linux: yes, sequential in one job. Windows: Release only |
| Sanitizer job | None (S3) |
| Non-C locale run | None; locale tests skip silently (S2) |
| Warnings as errors | None (`-Wall -Wextra -Wpedantic`, `/W4`); flags also apply to sqlite3.c and FTXUI because they are set before `FetchContent_MakeAvailable` |
| FetchContent integrity | SQLite: `URL_HASH SHA3_256` (good); FTXUI: full commit SHA (good). One host, no mirror (S4) |
| Action pinning | All three actions pinned by 40-char SHA with a version comment (good; SHAs not verified here) |
| Permissions | Workflow `contents: read`; `publish` alone has `contents: write`; `persist-credentials: false` everywhere |
| Secrets in release.yml | Only `github.token` in the `gh release create` step. Inputs passed through `env:` (no script injection). The CI keyring password is a throwaway constant |
| Provenance | None (S3) |
| Tests before packaging | Yes: ctest runs before the package step in both jobs, and `publish` needs both |
| Installer tests | Linux smoke runs (fixture tarball only); Windows `Test-InventatoryUpdate.ps1` is not run in CI (S3) |
| Platform source selection | Done once with `if(WIN32)` at `CMakeLists.txt:39-66`. Shared `UpdateService.cpp` and `Environment.cpp` use `#ifdef _WIN32` inside; there is no stub fallback for other OSes (acceptable, `find_package(PkgConfig REQUIRED)` fails early on macOS) |
| NDEBUG handling | No project policy; only the T8 target is affected (see above) |
| Install rules | Linux only, binary + docs + `.desktop` + icons; not exercised in CI |
| `/utf-8` and manifest | Not set by the project (S3 finding 4) |
| SQLite compile options | Defaults. Consider `SQLITE_OMIT_LOAD_EXTENSION` (no extension use; removes the `-ldl` dependency) and `SQLITE_DQS=0`. Not a defect |
