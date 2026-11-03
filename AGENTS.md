# Inventatory guide for coding agents

This file is the operating guide for coding agents. Other sources:

- [CONTRIBUTING.md](CONTRIBUTING.md): build dependencies and contribution rules.
- [docs/ui-guide.md](docs/ui-guide.md): binding rules for anything rendered.
- [docs/scanner-transport-security.md](docs/scanner-transport-security.md): the Scan R1
  transport protocol.

## Code layout

- `src/app.cpp` is the shell, `App.h` holds the application state, and `src/app/` holds
  the workflows. Do not split `App` or add a new state scheme for an ordinary feature.
- `src/ui/pages/` is page-local rendering and input. `src/ui/shared/` holds shared roles
  and formatting.
- `src/core/` owns models, SQLite, restore and the scanner protocol. Never duplicate a
  database or protocol rule in UI code.
- `src/import/`, `src/platform/` and `src/label_printer/` hold the importers, the OS code
  and the label printer.
- Keep `src/app/shell/AppNavigation.h` and the UI guide in step on the six destinations.

## Threading

- Document the owning thread of any new shared state. Use scoped locks.
- Never hold a mutex across a callback, network or printer I/O, or a long SQLite call.
- Settings, data directory, restore, scanner and quick-label state are reached from both
  the foreground and background callbacks. Check both paths.

## Persistence, backup and restore

- The data directory holds `inventory.db`, `activity.tsv`, `printer.conf`,
  `quick_labels.conf` and `inventatory_scanner.conf`. `settings.conf` lives in
  `LOCALAPPDATA` or `XDG_CONFIG_HOME` and never holds secrets.
- SQLite is the authority. Keep multi-step writes in one transaction. The UI never opens
  its own database path.
- Settings and quick-label writes are atomic (temp file, then replace). Validate a draft
  before it becomes active. A setting that affects a running service goes through the
  existing restart and apply path.
- A failed save, import, print or restore keeps the in-memory state so the user can retry.
- Backups carry each file's size and SHA-256 in a manifest and are validated in staging.
  Restore makes a pre-restore backup first and is never partly activated. Secrets and
  Scan R1 pairing data are never in a backup. After a restore, rotate the scanner token,
  clear replay state and remove the device identity.
- Activity and movement records are not inventory history. A migration never drops
  history.

## Label units and vendors

- Normalize units only in `normalizedUnitValue`
  (`src/label_printer/core/LabelPrinterDetails.cpp`), never in a vendor parser. No space
  before the unit, and the SI prefix that gives a mantissa of 1 to 999. Capacitance is the
  exception: F from 1 F, mF from 100 mF, then µF, nF, pF. Both ends of a range share one
  prefix. Values the rule does not own print as the vendor sent them.
- When two vendors name one package differently, add the pair to `canonicalPackageName`.
- Manufacturer names print in full when they fit, otherwise cut to the leading word
  before any smaller type. Short brand names (ST, TI, ADI) are tried last.
- A new vendor adds records to `tests/fixtures/tme-digikey-parity.tsv`. Only a real data
  difference may be exempted, with the reason.

Do not use this file as a reason to redesign the application. Understand the existing
path, keep its invariants, and make the smallest coherent change.

## Before editing

1. Read the relevant part of `README.md` and the applicable document under `docs/`.
2. Inspect the existing code and nearby tests. Search by visible behavior, persistence
   key, action key or protocol field, not by a new abstraction.
3. Find the owner of the state and its error and retry path. Keep the change inside
   that owner unless the behavior really crosses a boundary.
4. Check `git status` and the current diff. Do not overwrite unrelated user work or
   generated files.
5. Make one logical change at a time. Update focused tests and documentation when
   observable behavior or an invariant changes.

## Autonomy and reporting

- Commit, push, tag, open pull requests, or publish releases only when the user
  asks for that step.
- Do not discard, overwrite, or revert changes you did not make.
- Do not reset a device, flash hardware, or change stored credentials unless
  the user asks for it.
- Report verification by the level that actually ran (compiled, host or unit
  tests executed, or hardware and live integration executed). List the checks
  you could not run and why.

## Build and test

From the repository root, on Windows with Visual Studio 2026 C++ tools. Visual
Studio 2022 has also been used successfully:

```powershell
cmake -S . -B build
cmake --build build --config Release --target inventatory inventatory_background inventatory_tests inventatory_input_tests -- /m:1
ctest --test-dir build -C Release --output-on-failure
```

Install the Linux build dependencies listed in CONTRIBUTING.md, then build and test
Debug and Release:

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

`CMakeLists.txt` requires C++17 and pins fetched source content. Keep dependency
revisions and integrity hashes explicit; do not replace a pinned revision with a
moving branch or tag.

## Code conventions

- Follow the local two-space indentation, naming and header layout. Avoid
  unrelated formatting churn.
- Keep platform code behind the existing platform source selection and OS guards.
  Do not write parallel application implementations.
- Use the existing value types, `std::filesystem`, `std::optional`, RAII and the
  existing result and error-string conventions. No raw owning pointers, no naked
  `new`/`delete`, no new error framework for a local change.
- Do not mutate UI state or render from a worker thread.
- Every new first-party C, C++ or Python file starts with these two lines (after a
  shebang, `#` instead of `//` in Python):
  `// SPDX-FileCopyrightText: <year> Paweł Kwiatkowski` and
  `// SPDX-License-Identifier: GPL-3.0-only`. Third-party code keeps its own licence,
  is listed in `THIRD_PARTY_NOTICES.md` and never carries this header.
- Use prepared SQLite statements with bound parameters, and the existing
  `InventoryStore` and save helpers.

## Terminal UI

- Read `docs/ui-guide.md` before changing anything that is rendered. It is
  binding. In short: ASCII text only (no dots, arrows, chevrons, ticks, or bracketed
  values), fixed aligned columns, status as words, calm status colours that match the
  petrol-cyan base, and a plain-sentence header voice built from tested templates.
  Run its review checklist on every UI change.
- Pages and page-local input go in `src/ui/pages/`. Shared roles and formatting go in
  `src/ui/shared/AppUiShared.{h,cpp}`. Prefer the existing role helpers over new
  literal RGB colors.
- Register contextual commands through `currentActions()`. Keep labels, keys,
  action-sheet entries and direct-key behavior in sync. `App::Action::id` is
  identity: keep it when you change wording or layout, and never put user data,
  secrets or inventory contents in it.
- Register mouse targets through `App::target`. A target needs a keyboard
  equivalent or must be intentionally non-actionable. The UI is keyboard-first but
  fully usable with a mouse. Do not add a mouse-only workflow or a hidden focus trap.
- Render and hit-test overlays in reverse visual order.
- Settings panels carry settings and state only. No explainer notes, hint lines or
  descriptive copy under a setting. Status values that report real state are not
  explainers. Setup guidance belongs in the setup wizard, longer text in `docs/`.
- No explainers anywhere in the UI, not only in settings. Do not add hint lines,
  helper sentences, "how to" notes, placeholder explanations or descriptive copy under
  a field, button, form or panel. If something needs explaining, rename its label. State
  that reports something real (a count, an error after a failed action) is not an
  explainer. Show key hints only in the shared status line.
- Anything the user can type into or edit must look like an input: a filled box with its
  own background, visible when empty and when not focused, with the focused one in the
  selection colour. Plain text beside a label must never be editable. Plain text that
  cannot be edited must never look like a box.
- The supported minimum is 100x30 cells. Test fresh maximized-terminal captures at
  100x30, 120x30 and a wider size when a layout changes. Smaller terminals show the
  existing resize notice.
- Do not reintroduce the retired browser scanner.

## Secrets and the scanner boundary

- Store secrets only through the platform `CredentialStore` (Windows Credential
  Manager or Linux Secret Service). This includes the Scan R1 token and the DigiKey
  credential where applicable.
- Never put secrets in config files, logs, activity/history, SQLite snapshots,
  backup archives, error messages, action IDs, screenshots, commit messages,
  or device protocol diagnostics.
- Treat the scanner transport implementation in
  `src/core/scanner/InventatoryScanProtocol.cpp` and
  `src/platform/scanner/HttpServer.cpp` as a security boundary:
  - BLE provisioning sends a random 32-byte shared secret. It is never sent over HTTP.
  - HMAC-SHA-256 covers the exact method, path, device identity, counter and JSON body.
  - Keys are directional. Counters are monotonic and persist across restarts.
  - The protocol gives integrity and freshness, not confidentiality. Do not call it or
    build it as encrypted transport.
  - mDNS is for discovery only. Never bind the service publicly or remove the
    private-LAN and user-consent boundary.
  - Token rotation, reset, device identity and replay state change together and fail safely.
- Never log scanned codes, raw buffers, tokens, MAC material, authenticated bodies or
  credentials. Keep the CI `secret-logging` check passing.

## Testing expectations

Add or update a focused test for every changed persistence, import, history,
backup/restore, settings, printer, or Scan R1 behavior, including its failure
behavior. For protocol or replay changes, add positive and negative tests: wrong
method, path, body and device, a bad MAC, stale or replayed counters, restart
persistence, rotation and reset, malformed JSON, and a failing durable callback. For
UI changes, verify keyboard and mouse routes,
action-sheet presence, 100x30 layout, focus order, overlays and resize. The test
program does not replace that check.

Before handing off, run the Release build and CTest commands above, inspect
failures rather than weakening assertions, and review the final diff for unrelated
files or generated output.

## Releases and changelog

- Do not edit `CHANGELOG.md` in a feature branch. For a user-visible change, the
  completion summary proposes the entry you would add, in Keep a Changelog form and
  plain user terms. The maintainer rewrites it, and the approved wording goes in the
  pull request's Changelog section, copied as written. Release notes are collected
  from those sections into `CHANGELOG.md` at release time.
- `CHANGELOG.md` follows Keep a Changelog (`## [vX.Y.Z] - YYYY-MM-DD`). Entries
  describe only what a user sees or can do. Leave out refactors, renames, test
  changes, documentation edits, agent guidance, build or CI tweaks, and internal
  names. Work with no user-facing effect gets no entry. Write in plain user terms.
- When you bump the version or cut a release, add or update that version's
  section with the standard subsections (`### Added`, `### Changed`,
  `### Deprecated`, `### Removed`, `### Fixed`, `### Security`).
- A release with a meaningful change opens with a `**Highlights**` block directly
  under the version heading. A change is meaningful when it adds a major feature
  or supplier, changes a daily workflow, or needs users to act (for example a
  one-way database upgrade). Patch releases with small fixes have no block.
  Write two to five bullets, each with a bold lead-in and one plain sentence. The
  first names the change people will notice most. An upgrade warning gets its own
  bullet that starts with `**Before you upgrade:**`. Every highlighted change also
  appears in its subsection. While a release is open, keep the block under
  `## [Unreleased]`. Use plain Markdown only: bold, code spans and bullets.
- Keep release notes standard Markdown, without raw HTML or unbounded ASCII art,
  because the update wizard renders them.

To cut a release:

1. The required CI jobs pass: `linux`, `linux-runtime`, `linux-clang`,
   `linux-sanitizers`, `secret-logging` and `windows`.
2. Bump the version in `project(InventatorySoftware VERSION x.y.z)` in `CMakeLists.txt`.
3. Tag `vX.Y.Z`, or `vX.Y.Z-rc.N` for a release candidate.
4. The release notes come from the first that exists: `release-notes/<tag>.md`, the
   workflow input, the `CHANGELOG.md` section, then GitHub's generated notes.
5. Check that the published packages report the new version.

Leave the `SCAN_FIRMWARE_REPOSITORY` variable unset until the firmware repository is
public.

## Generated and local artifacts

Do not edit or commit generated or local output: `build/`, `build-*/`, `.vs/`,
`out/`, `dist/`, Debug/Release or architecture-specific build output, downloaded
FetchContent trees, test data, local Inventatory data, credentials, logs,
screenshots, or temporary files. If a release needs a generated file, change the
tracked source or script that produces it and verify the result locally.

## Commits and changelog

- Format: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `style`,
  `test`, `docs`, `build`, `ci`, `chore`, `revert`.
- Subject: imperative, lowercase after the colon, no full stop, 72 characters or fewer.
  Name the outcome, not the tool.
- Breaking change: `!` after the scope.
- Never put prompts, conversation ids, local paths, credentials, device ids or real
  inventory data in commits, branch names or pull requests.

Shared scopes: `build` (build system, packaging, CI), `docs` (files under `docs/`),
`project` (root files and GitHub templates), `release` (release process and notes).
The domain scopes for this repository are:

`app`, `background`, `bom`, `catalogue`, `core`, `device`, `digikey`, `history`,
`import`, `inventory`, `labels`, `platform`, `printer`, `racks`, `restore`,
`scan`, `search`, `settings`, `setup`, `startup`, `stock`, `storage`, `ui`, `update`.

Example: `fix(storage): preserve inventory history during restore`.

Do not commit generated output, unrelated cleanup, temporary checkpoints,
merge-noise messages, or development artifacts. Branch and pull-request names and descriptions describe the user-facing or maintenance outcome.

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
- [ ] `CHANGELOG.md` is not edited on this branch. The proposed entry is in the
      completion summary, and the approved wording is in the pull request's Changelog
      section. Version bumps have their own entry.
- [ ] Commit, branch and PR wording follows the commit rules above, and new source
      files carry the SPDX header.
