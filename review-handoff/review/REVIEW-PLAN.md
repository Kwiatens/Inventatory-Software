# Inventatory — Full Codebase Review Plan

Scope: all of `src/` (~45k lines), `tests/` (6.3k), `CMakeLists.txt`, `installer/`, `.github/workflows/`, `docs/`.
Emphasis: **Linux** (most escaped bugs), test coverage gaps, and code quality fit for an open-source codebase.
Baseline: `main` @ 1c75564, plus the uncommitted DigiKey-on-Linux change in the working tree. That change is the user's work. Reviewers read it but never modify it.

## Principles

1. **Read-only first, fixes later.** Reviewers do not edit anything. Fixes start only after the findings are verified and triaged with the user.
2. **Evidence or it didn't happen.** Every finding needs a `file:line`, a concrete failure scenario (inputs and state leading to a wrong result), and a proposed test that would catch it. Style opinions without a consequence are dropped.
3. **Independent verification.** Each candidate finding goes to a separate skeptical verifier whose job is to refute it. Where practical, the verifier proves it with a failing test or a sanitizer report. Only CONFIRMED and PLAUSIBLE findings reach the report.
4. **Severity before volume.** S1 = data loss, security, crash, or corrupt persistence. S2 = wrong behavior in a supported workflow, Linux/Windows divergence, race. S3 = edge case or poor error recovery. S4 = quality: dead code, duplication, over-long code, perf.
5. **Mine sibling bugs.** Recent escaped bugs show which bug classes to hunt. Each reviewer searches its slice for siblings of these:
   - locale-dependent parsing and formatting (`b528bfb`: unit search broken on Linux)
   - files rewritten when unchanged, non-idempotent startup side effects (`35f436b`, `1a14a57`)
   - process lifecycle and relaunch after update (`e8664e8`), background service takeover and forced stop (`eb87861`)
   - platform asymmetry: `#ifdef _WIN32` paths with no Linux equivalent or a weaker stub, Windows-only assumptions (`\r\n`, case-insensitive paths, `%LOCALAPPDATA%`, wide strings)
   - desktop and icon identity on Linux (`b636a51`, `97dd420`)
6. **Respect the invariants in AGENTS.md.** These are persistence and restore atomicity, keeping secrets out of logs and backups, the Scan R1 HMAC/counter/replay rules, the UI-thread-only mutation rule, and keyboard/mouse/action-sheet parity.
7. **Machine checks complement human-style reading.** These are an ASan+UBSan Debug build running the full test suite, and a strict Clang warning build (`-Wshadow -Wnull-dereference -Wthread-safety …`).

## Phases

| # | Phase | Who | Output |
|---|---|---|---|
| 0 | Baseline: GCC ASan/UBSan build + ctest; Clang strict-warning build + ctest | orchestrator (background) | `asan-*.log`, `clang-*.log` |
| 1 | Slice review (12 parallel reviewers, read-only) | Sonnet agents | `findings/<slice>.md` |
| 2 | Dedup + triage: merge cross-slice duplicates, discard opinions, assign severity | orchestrator | candidate list |
| 3 | Adversarial verification of every S1–S3 candidate (reproduce with a test/sanitizer where feasible) | Sonnet verifier agents | CONFIRMED / PLAUSIBLE / REFUTED |
| 4 | Report and user triage: which findings to fix, in what order | orchestrator + user | approved fix list |
| 5 | Fixes: test first (red → green), one logical change per commit, in isolated worktrees | Sonnet fix agents | branch with commits |
| 6 | Fix review: orchestrator reviews each diff; Debug+Release+ASan ctest; docs/CHANGELOG sync | orchestrator | ready-to-merge branch |

## Slices (Phase 1)

| ID | Slice | Main files | Specific focus |
|---|---|---|---|
| A | Linux system integration | `BackgroundControllerLinux`, `StartupRegistrationLinux`, `ConsoleLinux`, `Environment`, `AppIconsLinux.h`, `installer/*.sh` | systemd user units, XDG paths, signals, terminal raw mode/restore, idempotency, shell-script quoting |
| B | Linux scanner transport | `BleProvisioningServiceLinux`, `HttpServer*`, `MdnsService`, `SocketPlatform.h`, `CredentialStoreLinux` | BlueZ D-Bus lifetimes, GLib main loop threading, socket EINTR/EAGAIN/SIGPIPE, partial reads, bind address, libsecret errors |
| C | Scan R1 protocol (security boundary) | `core/scanner/*`, `app/scanner/*` | HMAC over the exact bytes, counter persistence, replay, rotation/reset, constant-time compare, durable-callback failure |
| D | Storage + history | `core/storage`, `core/history`, `app/persistence` (excluding backup) | transactions, rollback, prepared statements, schema migration, commit/snapshot/diff conflicts, retry path |
| E | Backup / restore / transfer | `core/transfer`, `app/persistence/AppBackupRestore` | manifest SHA-256, staging, partial activation, pre-restore backup, path traversal in archives, secrets excluded |
| F | Inventory domain | `core/inventory`, `core/query`, `core/parts`, `core/racks`, `core/bom` | locale (`std::stod`, `tolower`, `isdigit` with negative chars), UTF-8, numeric parsing, allocation edge cases |
| G | Import + DigiKey | `src/import`, `platform/digikey` (incl. the uncommitted Linux transport), `app/import`, `app/bom` | CSV quoting/BOM/CRLF, KiCad parsing, curl error handling and timeouts, OAuth token handling, JSON robustness |
| H | Label printing | `src/label_printer`, `app/labels`, `app/racks/AppRackPrinting*` | CUPS on Linux, temp files, 203 dpi math, config atomic write |
| I | App shell, runtime, threading, updater | `app.cpp`, `App.h`, `app/shell`, `app/AppUpdate.cpp`, `UpdateService`, `app/settings`, `app/workspace` | worker → UI queue discipline, mutex coverage, shutdown ordering, Linux update/relaunch, settings atomic writes |
| J | UI pages + action registry | `src/ui/**` | action-sheet ↔ direct-key parity, mouse targets with keyboard equivalents, 100x30 layout, focus traps, UTF-8 width |
| W | Windows platform layer | `BackgroundController`, `BackgroundLauncher`, `StartupRegistration`, `Console`, `CredentialStore`, `BleProvisioningService` (WinRT), `LabelPrinterWindows`, `DigiKeyTransport` (WinHTTP), every `#ifdef _WIN32`, `installer/*.ps1` | HANDLE/COM leaks, CP_ACP vs UTF-8 (non-ASCII profile paths), CreateProcess quoting, min/max macros, WinRT callback threading, self-replacement of the running exe. Can't be compiled here, so reading only |
| K | Tests, build, CI | `tests/*`, `CMakeLists.txt`, `.github/workflows/release.yml` | coverage-gap map per module, weak/tautological asserts, Linux CI coverage, pinned deps, warnings policy |

Every reviewer also reports **S4 quality findings** in a separate section: dead code, duplicated logic, over-long functions, needless copies, O(n²) loops. This list is ranked lower and fixed opportunistically.

## Finding format (all agents)

```
### [S1|S2|S3|S4] <short title>
- Location: path:line[-line]
- Category: correctness | security | concurrency | persistence | linux-portability | ui-parity | perf | quality | test-gap
- Failure scenario: concrete inputs/state → wrong output/crash
- Evidence: the exact code lines and why they're wrong (quote ≤10 lines)
- Confidence: high | medium | low
- Proposed test: what assertion in tests/inventatory_tests.cpp would catch it
- Proposed fix (sketch): 1–3 lines
```

## Exit criteria

- All S1/S2 findings are verified and either fixed with a regression test or explicitly deferred by the user.
- Debug, Release and ASan/UBSan `ctest` all pass on Linux. The diff contains no generated files or secrets.
- Docs and CHANGELOG are updated where behavior changed. Commits follow the AGENTS.md convention.

## Execution constraint: at most 2 agents at a time (Pro plan)

Slices run in waves of two. The next wave starts only after the user says to continue:

| Wave | Agent 1 | Agent 2 |
|---|---|---|
| 1 | A+B — Linux platform (system, transport, credentials) | I — shell, threading, settings, updater |
| 2 | C+E — Scan R1 protocol + backup/restore | D+F — storage/history + inventory domain (locale siblings) |
| 3 | G+H — imports/DigiKey + label printing | W — Windows platform |
| 4 | J — UI pages | K — tests/build/CI |
| 5 | Verification of S1/S2 findings, batched into ≤ 2 verifiers | |
