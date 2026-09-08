# Inventatory coding-agent guide

This file is the repository-level operating guide for coding agents working on
Inventatory. It describes the current codebase and its supported behavior. Do
not treat it as a reason to redesign the application: first understand the
existing path, preserve its invariants, and make the smallest coherent change.

## Project overview

Inventatory is a Windows-only C++17 terminal inventory application in public
beta. The supported build and test environment is Visual Studio 2022 with the
C++ workload and CMake 3.20 or newer. The application has a foreground
terminal UI, an optional Windows background/tray process, local SQLite
persistence, import workflows, label printing, and the authenticated physical
Inventatory Scan R1 device service.

The user-facing interface is the terminal UI. The current normal workspace navigation is:

`1 Home`, `2 Stock`, `3 Racks`, `4 Import`, `5 Projects`, `6 History`,
`7 Settings`.

`docs/user-guide.md` and the implemented code describe this seven-destination
navigation. The older six-destination list in `docs/ui-redesign.md` predates
the History page; keep those documents synchronized when changing navigation.

## Before editing

1. Read the relevant section of `README.md` and the applicable document under
   `docs/` before changing behavior.
2. Inspect the existing implementation and nearby tests. Search by the visible
   behavior, persistence key, action key, or protocol field rather than starting
   from a new abstraction.
3. Identify the owner of the state and the existing error/retry path. Keep a
   change inside that owner unless the behavior genuinely crosses a boundary.
4. Check `git status` and the current diff. Do not overwrite unrelated user
   work or generated files.
5. Prefer one logical behavior change at a time. Update focused tests and
   documentation when the observable behavior or an invariant changes.

Repository-wide instructions belong here. A future nested `AGENTS.md` may add
more specific rules for a subtree, but it must not silently contradict the
security, persistence, or UI invariants below.

## Repository map and ownership

- `src/app.cpp` owns the application shell, page selection, top-level input
  routing, terminal sizing, render loop, and dispatch of queued background
  results.
- `src/App.h` defines the central application state and the interfaces shared
  by the shell, pages, and actions.
- `src/app/` owns cross-page workflows grouped by feature: persistence,
  workspace, inventory, racks, scanner, imports, BOM, labels, settings, and
  shell/runtime coordination.
- `src/ui/pages/` owns page rendering and page-local input. Keep page-specific
  presentation and input here rather than growing the shell.
- `src/ui/shared/` owns shared visual roles, formatting, layout helpers, and
  common widgets. Reuse these helpers when the current code supports the
  needed role.
- `src/ui/ActionRegistry.cpp` owns contextual action registration. Actions
  shown in the action sheet and actions reachable by direct keyboard input must
  come from the same `currentActions()` definitions.
- `src/core/` owns inventory models, SQLite persistence, settings-related
  transfer/restore logic, history, import-domain data, and Scan R1 protocol
  logic. Do not duplicate database or protocol rules in UI pages.
- `src/import/` owns file-format import and BOM/project matching workflows.
- `src/platform/` owns Windows Credential Manager, HTTP/mDNS/BLE, startup and
  background-process integration, updater, and other OS services.
- `src/label_printer/` owns printer discovery/configuration and label output.
- `tests/inventatory_tests.cpp` is the current core/integration test executable.
  It is intentionally a single assert-based test program; add focused cases
  there unless a new test target is justified by the change.
- `docs/` is product and contributor documentation. `installer/` contains
  release/install/uninstall scripts. `.github/workflows/` contains CI and
  packaging automation.

The application currently has a large central `App` state/controller. Do not
split it or introduce a new state-management architecture as a prerequisite
for an ordinary feature. When touching it, preserve the existing ownership
boundaries and extract only a narrow, behavior-preserving unit when that makes
the change safer.

## Build and test

From the repository root, with the required Windows tools installed:

```powershell
cmake -S . -B build
cmake --build build --config Release --target inventatory inventatory_background inventatory_tests -- /m:1
ctest --test-dir build -C Release --output-on-failure
```

For a focused core test iteration, build and run the `inventatory_tests` target.
Use `--output-on-failure` so a failing test retains its assertion output. The
same Release build and CTest sequence is used by
`.github/workflows/release.yml`.

`run.ps1` is the supported convenience launcher for local use. Do not assume
that a stale existing build directory represents the current source. If it is
locked, inaccessible, or generated by a different toolchain, use a separate
ignored out-of-tree directory such as `build-agent` rather than deleting or
repairing a user's build directory.

`CMakeLists.txt` requires C++17, enables `/W4 /permissive- /MP` under MSVC and
the corresponding warning set on other compilers, and pins fetched source
content. Keep dependency revisions and integrity hashes explicit; do not
replace a pinned revision with a moving branch or tag.

## C++ and implementation conventions

- Follow the local two-space indentation, brace placement, naming, and header
  organization. Avoid unrelated formatting churn.
- Keep platform-specific code behind the existing Windows guards and platform
  classes. This is not a cross-platform product; do not add portability layers
  unless the feature requires one.
- Prefer the existing value types, `std::filesystem`, `std::optional`, RAII,
  smart ownership, and existing result/error-string conventions. Do not add
  raw owning pointers, naked `new`/`delete`, or a new exception/error framework
  for a local change.
- Release resources on every path. Use scoped lock guards for mutexes and keep
  lock ownership obvious. Do not hold a mutex while calling unknown callbacks,
  network code, printer code, or long-running SQLite work unless the existing
  invariant explicitly requires serialization.
- Preserve the current background pattern: worker threads/futures perform
  slow work, then queue results for the UI/application loop. Do not mutate UI
  state or render from a worker thread. When adding shared state, document its
  owning thread and protect all reads and writes with the same existing mutex
  or queue discipline.
- Use prepared SQLite statements and bound parameters. Keep multi-step writes
  transactional and follow the existing `InventoryStore`/application save
  helpers instead of opening ad hoc database paths from UI code.
- Return useful user-facing errors and preserve recoverability. A failed save,
  import, print, or restore should not silently discard an in-memory state that
  the existing workflow can retry.

## Terminal UI and interaction model

- Render pages and page-local input in `src/ui/pages/`.
- Put reusable semantic visual roles and formatting in
  `src/ui/shared/AppUiShared.{h,cpp}`. Prefer helpers such as the existing
  canvas, surface, focus, muted, warning, success, and error roles over new
  literal RGB colors.
- Register contextual commands through `currentActions()`. Keep their visible
  labels, keyboard triggers, action-sheet entries, and direct-key behavior in
  sync. Treat `App::Action::id` as identity: preserve an existing action's
  identity when changing its wording or layout, and do not put user data,
  secrets, or inventory contents in IDs.
- Register mouse targets through `App::target`. A target must have a keyboard
  equivalent or be intentionally non-actionable. Preserve focus movement,
  Enter activation, action-sheet discoverability, and mouse parity.
- Interactive overlays are rendered and hit-tested in reverse visual order so
  the visually topmost control receives the event.
- Keep the terminal UI usable at the supported minimum of 100x30 cells. Test
  fresh maximized-terminal captures at 100x30, 120x30, and a wider size when a
  layout changes. Smaller terminals must show the existing resize notice
  rather than a clipped operational screen.
- The UI is keyboard-first but fully mouse-usable. Do not add a mouse-only
  workflow, hidden focus trap, or action that is absent from the contextual
  action sheet without documenting why it is intentionally non-command UI.
- Keep onboarding, normal shell, resize notice, overlays, and transient
  messages consistent with the existing four-region shell. Do not reintroduce
  the retired browser scanner.

## State, threading, and cross-page workflows

The foreground `App` owns the active UI state. Background HTTP, BLE, mDNS,
printer, import-network, and tray work must use the existing queues, futures,
callbacks, and mutexes to cross into that state. A new worker must not call
rendering code or directly mutate state that the UI reads without the existing
ownership/locking discipline.

When changing settings, data-directory selection, restore, scanner state, or
quick-label state, inspect both the foreground path and the background device
callback path. Update every read/write under the corresponding mutex; do not
assume that a lock around only the final UI assignment protects an object that
another worker reads concurrently.

## Persistence, history, backup, and restore

- The selected data directory contains `inventory.db`, `activity.tsv`,
  `printer.conf`, `quick_labels.conf`, and `inventatory_scan.conf` as
  applicable. `%LOCALAPPDATA%\Inventatory\settings.conf` contains local
  application settings, not inventory data or secrets.
- SQLite inventory data is authoritative for current stock, racks, BOM/project
  data, device-event inbox data, movements, commits, and inventory history.
  Preserve the existing commit/snapshot/diff model and its conflict checks.
- Use the existing store/application save paths. Keep transaction boundaries,
  rollback behavior, initial-commit behavior, and the in-memory retry path
  intact.
- Backups must preserve inventory history and the documented inventory data.
  They must include manifest size/SHA-256 validation and be validated in a
  staging location before activation.
- Restore must create/use the pre-restore backup, validate before activation,
  and leave the active workspace unchanged if validation or activation fails.
  Do not partially activate a restore.
- Secrets and Scan R1 pairing/device data are excluded from backups. After a
  successful restore, follow the documented token rotation, replay-state
  clearing, and device-identity removal behavior.
- Do not reinterpret historical activity or movement records as inventory
  history, and do not silently discard historical records during a current-data
  migration.

## Settings and secret handling

- Store ordinary settings through the versioned settings path under
  `%LOCALAPPDATA%\Inventatory\settings.conf`. Store inventory files under the
  selected data directory.
- Store secrets through `CredentialStore`, backed by Windows Credential
  Manager. This includes the Scan R1 token and the DigiKey credential where
  applicable.
- Never put secrets in config files, logs, activity/history, SQLite snapshots,
  backup archives, error messages, action IDs, screenshots, commit messages,
  or device protocol diagnostics.
- Preserve the existing atomic temp-file/replacement behavior for settings and
  quick-label writes. Validate drafts before activation and keep cancel/failed
  save behavior consistent with the current settings workflow.
- When a settings change affects a running bridge/device service, use the
  existing restart/apply path and make the resulting status visible to the
  user. Do not silently leave a service using stale credentials or paths.

## Inventatory Scan R1 security invariants

Treat `docs/scanner-transport-security.md` and the code in
`src/core/scanner/InventatoryScanProtocol.cpp` and
`src/platform/scanner/HttpServer.cpp` as a
security boundary. Changes must preserve all of the following:

- BLE provisioning is physically verified and provisions a random 32-byte
  shared secret; the secret is stored in Credential Manager/NVS and never sent
  through HTTP.
- Requests authenticate the exact method, path, device identity, monotonic
  counter, and exact JSON body with HMAC-SHA-256.
- PC and device use directional derived keys. Counters are monotonic and
  persisted across restart; device-side reservations and PC-side accepted
  counters provide replay resistance.
- The protocol authenticates integrity and freshness but does not provide
  payload confidentiality. Do not describe or implement it as encrypted
  transport without a deliberate protocol design and migration.
- mDNS is discovery only. Do not weaken authentication, bind the service
  publicly, add port-forwarding assumptions, or remove the private-LAN/user
  consent boundary.
- Token rotation, clear/reset, device identity changes, and replay-state
  changes must remain coordinated and fail safely.

For protocol or replay changes, add positive and negative tests for wrong
method/path/body/device, bad MAC, stale/replayed counters, restart persistence,
rotation/reset, malformed JSON, and durable-callback failure. Do not log tokens,
MAC material, full authenticated bodies, or credentials while debugging.

## Testing expectations

Add or update a focused test for every changed persistence, import, history,
backup/restore, settings, printer, or Scan R1 behavior. Prefer tests that
exercise the public helper/workflow and verify failure behavior, not only the
happy path.

For concurrency-sensitive changes, test duplicate/replayed requests, restart
state, queue ordering, and shutdown behavior where practical. For UI changes,
verify keyboard and mouse routes, action-sheet presence, minimum terminal
layout, focus order, overlays, and resize behavior. The current test program
does not replace this UI verification.

Before handing off a change, run the Release build and CTest commands above,
inspect failures rather than weakening assertions, and review the final diff
and status for unrelated files or generated output.

## Generated, local, and vendor artifacts

Do not edit or commit generated/local output such as `build/`, `build-*/`,
`.vs/`, `out/`, `dist/`, Debug/Release or architecture-specific build output,
downloaded FetchContent trees under a build directory, test data, local
Inventatory data, credentials, logs, screenshots, or temporary files. The
tracked source, documentation, installer scripts, and workflow files are the
reviewable inputs. If a generated file is needed for a release, update the
tracked source/script that produces it and verify the generated result locally.

## Change and commit standard

Use one logical change per commit. Allowed types are:

`feat`, `fix`, `refactor`, `perf`, `style`, `test`, `docs`, `build`, `ci`,
`chore`, `revert`.

The scope is required for normal changes and should be short, lowercase, and
meaningful, for example `inventory`, `storage`, `ui`, `setup`, `startup`,
`import`, `scan`, `platform`, `printer`, `release`, `build`, `ci`, or `project`.

Commit subjects must use imperative present tense, start with a lowercase word
after the colon, omit a trailing period and vague wording, stay concise (ideally
72 characters or fewer), and describe the user or maintenance outcome rather
than the editor or AI used. Never include `WIP`, temporary debugging language,
credentials, or local paths.

Use a body after a blank line when the non-obvious reason, compatibility
constraint, security consequence, migration, or testing matters. Wrap body
text near 100 columns. Breaking changes use `!` after the scope and include a
`BREAKING CHANGE:` footer.

Examples:

```text
feat(import): add KiCad BOM project matching
fix(storage): preserve inventory history during restore
refactor(ui): move shared detail formatting into helpers
test(scan): cover replayed device counters
docs(project): document release installation
build(cmake): pin the bundled SQLite amalgamation
```

Do not commit generated output, unrelated cleanup, temporary checkpoints,
merge-noise messages, or development artifacts. Branch and pull-request names
and descriptions should describe the user-facing or maintenance outcome and
must not expose local tooling or an AI agent.

## Pre-commit checklist

- [ ] Relevant source and `docs/` ownership boundaries were inspected first.
- [ ] The change preserves current persistence, history, restore, secret, and
      Scan R1 invariants.
- [ ] New or changed behavior has focused tests and documented failure paths.
- [ ] UI changes preserve keyboard parity, mouse targets, action-sheet
      discoverability, semantic roles, focus order, and 100x30 usability.
- [ ] Release build and `ctest --output-on-failure` were run, or the exact
      environment blocker is reported.
- [ ] No credentials, secrets, local paths, generated output, or unrelated
      files entered the diff.
- [ ] Commit/PR wording follows the standard above.
