# Inventatory coding-agent guide

This file is the repository-level operating guide for coding agents working on
Inventatory. It describes the current codebase and its supported behavior. Do
not treat it as a reason to redesign the application: first understand the
existing path, preserve its invariants, and make the smallest coherent change.

Rules shared with the scanner firmware repository (commit and changelog format,
licensing headers, agent autonomy, secrets and logging, verification reporting)
are in [docs/project-standards.md](docs/project-standards.md). This file adds the
rules for the application code.

## Project overview

Inventatory is a C++17 terminal inventory application in public beta with
separate Windows and Linux releases. Windows remains supported through Visual
Studio 2022 and CMake 3.20 or newer; native Linux targets x86-64 distributions with
glibc 2.34 or newer. The release is built and tested with GCC 11 in AlmaLinux 9, and
`tools/check-linux-abi.sh` enforces that floor. CI also builds with Clang and runs the
sanitizers on Ubuntu 24.04, and checks the release binary on Ubuntu 22.04, Debian 12 and AlmaLinux 9
with `tools/check-linux-runtime.sh`. Both need CMake 3.20 or newer. The application has a shared
foreground terminal UI, local SQLite persistence, import workflows, label
printing, and the authenticated physical Inventatory Scan R1 device service.
Windows keeps its background/tray process; Linux uses a per-user systemd
background service.

The user-facing interface is the terminal UI. The current normal workspace navigation is:

`1 Stock`, `2 Racks`, `3 Import`, `4 Projects`, `5 History`, `6 Settings`.

The implemented code and UI style guide describe this six-destination navigation; keep them synchronized when changing it.

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

## Autonomy and reporting

- Commit, push, tag, open pull requests, or publish releases only when the user
  asks for that step.
- Do not discard, overwrite, or revert changes you did not make.
- Report verification by the level that actually ran (compiled, host or unit
  tests executed, or hardware and live integration executed). List the checks
  you could not run and why.

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
- `src/platform/` owns credential storage, HTTP/mDNS/BLE, startup and
  background-process integration, updater, and other OS services. CMake selects
  Windows implementations or the Linux POSIX/BlueZ/Secret Service/CUPS
  implementations without duplicating application logic.
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

From the repository root, on Windows with Visual Studio 2026 C++ tools. Visual
Studio 2022 has also been used successfully:

```powershell
cmake -S . -B build
cmake --build build --config Release --target inventatory inventatory_background inventatory_tests inventatory_input_tests -- /m:1
ctest --test-dir build -C Release --output-on-failure
```

Install the Linux build dependencies listed in CONTRIBUTING.md for your
distribution, then build and test Debug and
Release configurations:

```sh
cmake -S . -B build-linux-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug --parallel
ctest --test-dir build-linux-debug --output-on-failure
cmake -S . -B build-linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release --parallel
ctest --test-dir build-linux-release --output-on-failure
```

For a focused core test iteration, build and run the `inventatory_tests` target;
`inventatory_tests --list` shows its named tests and `--filter <text>` runs only
the matching ones. Register a new test in the table at the end of
`tests/inventatory_tests.cpp` under the area it covers, and keep it independent
of the other tests. Use `--output-on-failure` so a failing test retains its
assertion output. The same Release build and CTest sequence is used by
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
- Keep platform-specific code behind platform source selection, existing OS
  guards, and narrow platform classes. Share domain logic and avoid parallel
  application implementations.
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

- Read `docs/ui-style-guide.md` before changing anything that is rendered. It is binding for
  glyphs, alignment, controls, colour, status, unsaved-change display, and guided workflows.
  In short: ASCII text only (no dots, arrows, chevrons, ticks, or bracketed values), fixed
  aligned columns, status as words, calm status colours that match the petrol-cyan base, and a
  plain-sentence header voice built from tested templates.
  Run its review checklist on every UI change.
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
- Settings panels carry settings and state only. Do not add explainer notes, hint
  lines, or descriptive copy under a setting or section; they add clutter. Status
  and empty-state values that report real state (for example "No printer queues
  detected") are not explainers. Setup guidance belongs in the setup wizard and
  longer explanations belong in `docs/`.
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
  applicable. `%LOCALAPPDATA%\Inventatory\settings.conf` on Windows or
  `$XDG_CONFIG_HOME/Inventatory/settings.conf` on Linux contains local
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
  `%LOCALAPPDATA%\Inventatory\settings.conf` on Windows or
  `$XDG_CONFIG_HOME/Inventatory/settings.conf` (default `~/.config/Inventatory`)
  on Linux. Store inventory files under the selected data directory.
- Store secrets through the platform `CredentialStore`: Windows Credential
  Manager or Linux Secret Service. This includes the Scan R1 token and the
  DigiKey credential where applicable.
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

Treat the scanner transport implementation in
`src/core/scanner/InventatoryScanProtocol.cpp` and
`src/platform/scanner/HttpServer.cpp` as a
security boundary. Changes must preserve all of the following:

- BLE provisioning is physically verified and provisions a random 32-byte
  shared secret; the desktop stores it through Credential Manager on Windows
  or Secret Service on Linux, the scanner stores it in NVS, and it is never
  sent through HTTP.
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

## Releases and changelog maintenance

- `CHANGELOG.md` in the repository root tracks version history following the
  Keep a Changelog convention (`## [vX.Y.Z] - YYYY-MM-DD`).
- Changelog entries describe only what a user of the application sees or can do:
  features, behaviour changes, fixes, and removals that affect them. Do not record
  internal work such as refactors, helper or file renames, test changes, documentation
  edits, agent guidance, build or CI tweaks, or the names of internal functions and
  classes. Write each entry in plain user terms, not as a list of code changes.
- When bumping the application version or cutting a release, add or update the
  corresponding version section in `CHANGELOG.md` using standard subsections
  (`### Added`, `### Changed`, `### Deprecated`, `### Removed`, `### Fixed`,
  `### Security`).
- A release with a meaningful change opens with a `**Highlights**` block, placed
  directly under the version heading and above the subsections. A change is
  meaningful when it adds a major feature or supplier, changes a workflow people
  use every day, or needs users to act, such as a one-way database upgrade.
  Patch releases with only small fixes have no block. Write two to five bullets:
  each has a bold lead-in and one plain sentence, and the first names the change
  people will notice most. Put an upgrade warning in its own bullet that starts
  with `**Before you upgrade:**`. The block summarises the subsections below it
  and does not replace them, so every highlighted change also appears in its
  subsection. While a release is open, keep the block under `## [Unreleased]`
  and move it with the other entries. The GitHub release and the update wizard
  both show it, so use plain Markdown only: bold, code spans and bullets.
- The release workflow (`.github/workflows/release.yml`) automatically extracts
  the section matching the release tag (`vX.Y.Z`) to populate the GitHub Release
  body and update asset metadata. A tag-specific override can also be provided
  via `release-notes/<tag>.md` or the `workflow_dispatch` release notes input.
- If no manual entry exists for a tag, the pipeline falls back to GitHub's
  generated notes so publishing is never blocked unexpectedly.
- The update wizard in the terminal application renders Markdown release notes
  using semantic styling (`uiAccentColor()` for section headers, `uiLinkColor()`
  for bullet points, and hanging indentation for wrapped items). Keep release
  notes clean, standard Markdown without raw HTML or unbounded ASCII art.

## Vendor data and label units

Label text is built from supplier records (TME, DigiKey and any vendor added later), and the same part must print the
same way whichever vendor supplied it. These rules apply to every vendor, not only the ones already supported.

- Units are normalised once, in the shared label code, not in a vendor's parser. A parser passes the vendor's value
  through with its unit, and never prints a scaled or base-unit form of its own. `normalizedUnitValue`
  (`src/label_printer/core/LabelPrinterDetails.cpp`) gives every quantity one form:
  - the figure has no space before its unit: `16V`, `600mV`, `1.5µH`, `26mΩ`;
  - the SI prefix is the one that leaves a mantissa from 1 to 999, so `0.25W` and `250 mW` both print `250mW`, and
    `0.6kV` prints `600V`;
  - capacitance is the exception, because electrolytics are written in µF: F from 1 F, mF from 100 mF, µF below that,
    then nF and pF (`4700µF`, `100nF`);
  - a range has one prefix for both ends (`3.8 to 32V`, `100 to 600mV`).
  A value the rule does not own, such as `Ø5x11mm`, `±20%`, `2000h` or `-40 to 85°C`, is printed as the vendor sent it.
- Package names: when two vendors name one package differently, add the pair to the alias table in
  `canonicalPackageName`, so both print the one name. A lead style (`Radial`, `Axial`) is not a package. A cylinder
  prints its diameter (`Ø5mm`), because the vendors do not both give a body length.
- Manufacturer names: print the full name when it fits at the largest size. Otherwise cut it to its leading word
  (`Panasonic Industry` prints `Panasonic`) before printing any name in smaller type. The short brand names (ST, TI, ADI)
  are tried last.
- A new vendor is covered by adding its records to the parity fixture (`tests/fixtures/tme-digikey-parity.tsv`) and
  checking the difference list. A difference in units or package spelling is a rule to fix here. Only a real data
  difference, such as a different test point, may be exempted, and the exemption gives the reason.

## Generated, local, and vendor artifacts

Do not edit or commit generated/local output such as `build/`, `build-*/`,
`.vs/`, `out/`, `dist/`, Debug/Release or architecture-specific build output,
downloaded FetchContent trees under a build directory, test data, local
Inventatory data, credentials, logs, screenshots, or temporary files. The
tracked source, documentation, installer scripts, and workflow files are the
reviewable inputs. If a generated file is needed for a release, update the
tracked source/script that produces it and verify the generated result locally.

## Commits and changelog

The commit format (types, shared scopes, subject and body rules, breaking
changes), AI co-author trailers, and the changelog rules are in
[docs/project-standards.md](docs/project-standards.md). The domain scopes for this
repository are:

`app`, `background`, `bom`, `catalogue`, `core`, `device`, `digikey`, `history`,
`import`, `inventory`, `labels`, `platform`, `printer`, `racks`, `restore`,
`scan`, `search`, `settings`, `setup`, `startup`, `stock`, `storage`, `ui`, `update`.

Examples:

```text
feat(import): add KiCad BOM project matching
fix(storage): preserve inventory history during restore
refactor(ui): move shared detail formatting into helpers
test(scan): cover replayed device counters
docs(release): document release installation
ci(build): pin the release workflow to a full commit SHA
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
- [ ] Version bumps include a corresponding entry in `CHANGELOG.md`.
- [ ] Commit, branch and PR wording follows
      [docs/project-standards.md](docs/project-standards.md), and new source
      files carry the SPDX header.
