# Architecture

This guide describes how the Inventatory code is laid out and which rules
hold it together. Read it before you change behavior. For build steps and
contribution rules, see [CONTRIBUTING.md](../CONTRIBUTING.md). For the rules
shared with the scanner firmware repository, see
[project-standards.md](project-standards.md). For anything drawn on screen,
[ui-style-guide.md](ui-style-guide.md) is binding.

The general approach: understand the existing path first, keep its
invariants, and make the smallest change that works. The sections below name
those invariants.

## Overview

Inventatory is a C++17 terminal inventory application in public beta, with
separate Windows and Linux releases.

- Windows is built with Visual Studio 2022 or newer and CMake 3.20 or newer.
- Native Linux targets x86-64 distributions with glibc 2.34 or newer. The
  release is built and tested with GCC 11 in AlmaLinux 9.
  `tools/check-linux-abi.sh` enforces the glibc floor.
- CI also builds with Clang and runs the sanitizers on Ubuntu 24.04. It
  checks the release binary on Ubuntu 22.04, Debian 12 and AlmaLinux 9 with
  `tools/check-linux-runtime.sh`.

The application has one foreground terminal UI, local SQLite persistence,
import workflows, label printing, and the authenticated service for the
physical Inventatory Scanner R1 device. Windows keeps a background/tray process.
Linux uses a per-user systemd background service.

The user-facing interface is the terminal UI. Normal workspace navigation has
six destinations:

`1 Stock`, `2 Racks`, `3 Import`, `4 Projects`, `5 History`, `6 Settings`.

The code (`src/app/shell/AppNavigation.h`) and the UI style guide both
describe this list. Keep them in step when you change it.

## Repository map and ownership

- `src/app.cpp` owns the application shell: page selection, top-level input
  routing, terminal sizing, the render loop, and dispatch of queued background
  results.
- `src/App.h` defines the central application state and the interfaces shared
  by the shell, pages, and actions.
- `src/app/` holds cross-page workflows grouped by feature: persistence,
  workspace, inventory, racks, scanner, imports, BOM, labels, settings, and
  shell/runtime coordination.
- `src/ui/pages/` holds page rendering and page-local input. Keep page-specific
  presentation and input here. Do not grow the shell with it.
- `src/ui/shared/` holds shared visual roles, formatting, layout helpers, and
  common widgets. Reuse them when they cover the role you need.
- `src/ui/ActionRegistry.cpp` registers contextual actions. Actions shown in
  the action sheet and actions reached by direct keyboard input must come from
  the same `currentActions()` definitions.
- `src/core/` holds inventory models, SQLite persistence, settings-related
  transfer and restore logic, history, import-domain data, and the Scan R1
  protocol logic. Do not duplicate database or protocol rules in UI pages.
- `src/import/` holds file-format import and BOM/project matching.
- `src/platform/` holds credential storage, HTTP/mDNS/BLE, startup and
  background-process integration, the updater, and other OS services. CMake
  picks the Windows implementations or the Linux POSIX, BlueZ, Secret Service
  and CUPS implementations. Application logic is not duplicated for either.
- `src/label_printer/` holds printer discovery and configuration, and label
  output.
- `tests/inventatory_tests.cpp` is the core and integration test program. It
  is one assert-based executable on purpose. Add focused cases there unless a
  new test target is justified.
- `docs/` is product and contributor documentation. `installer/` has the
  release, install and uninstall scripts. `.github/workflows/` has CI and
  packaging automation.

The central `App` class is large. It holds state and acts as controller. Do
not split it, and do not add a new state-management scheme, as a prerequisite
for an ordinary feature. When you touch it, keep the existing ownership
boundaries. Extract a unit only when it is narrow, keeps behavior the same,
and makes the change safer.

## State and threading

The foreground `App` owns the active UI state. Background HTTP, BLE, mDNS,
printer, import-network and tray work reaches that state only through the
existing queues, futures, callbacks and mutexes. The pattern is:

- Worker threads or futures do the slow work.
- They queue the result for the UI and application loop.
- A worker never calls rendering code and never mutates state the UI reads
  without the existing ownership and locking rules.

Keep these points when you change threaded code:

- When you add shared state, document which thread owns it. Protect all reads
  and writes with the same mutex or queue.
- Use scoped lock guards and keep lock ownership obvious.
- Do not hold a mutex while calling unknown callbacks, network code, printer
  code or long SQLite work, unless the existing invariant needs it.
- When you change settings, data-directory selection, restore, scanner state or
  quick-label state, check both the foreground path and the background device
  callback path. Update every read and write under the matching mutex. A lock
  around only the final UI assignment does not protect an object that another
  worker reads at the same time.
- Use prepared SQLite statements with bound parameters. Keep multi-step writes
  in a transaction. Go through `InventoryStore` and the application save
  helpers. Do not open ad hoc database paths from UI code.
- Return useful errors and stay recoverable. A failed save, import, print or
  restore must not silently throw away in-memory state that the workflow can
  retry.

## Persistence, history, backup and restore

- The selected data directory holds `inventory.db`, `activity.tsv`,
  `printer.conf`, `quick_labels.conf` and `inventatory_scanner.conf`, as
  applicable.
- Local application settings live in `%LOCALAPPDATA%\Inventatory\settings.conf`
  on Windows and `$XDG_CONFIG_HOME/Inventatory/settings.conf` on Linux. They
  are not inventory data and hold no secrets.
- SQLite is the authority for current stock, racks, BOM and project data,
  device-event inbox data, movements, commits and inventory history. Keep the
  commit, snapshot and diff model and its conflict checks.
- Use the existing store and save paths. Keep transaction boundaries, rollback
  behavior, initial-commit behavior and the in-memory retry path intact.
- Backups keep inventory history and the documented inventory data. They carry
  manifest size and SHA-256 checks. They are validated in a staging location
  before activation.
- Restore creates or uses the pre-restore backup and validates before it
  activates anything. If validation or activation fails, the active workspace
  stays unchanged. Never activate a restore partly.
- Secrets and Scan R1 pairing and device data are left out of backups. After a
  successful restore, follow the documented token rotation, replay-state
  clearing and device-identity removal.
- Do not read old activity or movement records as inventory history. Do not
  silently drop historical records during a migration of current data.

See also [backup-restore.md](backup-restore.md).

## Settings and secrets

- Ordinary settings go through the versioned settings path named above, with
  `~/.config/Inventatory` as the Linux default. Inventory files go in the
  selected data directory.
- Secrets go through the platform `CredentialStore`: Windows Credential Manager
  or Linux Secret Service. That covers the Scan R1 token and, where it applies,
  the DigiKey credential.
- Never put secrets in config files, logs, activity or history, SQLite
  snapshots, backup archives, error messages, action IDs, screenshots, commit
  messages, or device protocol diagnostics.
- Keep the atomic temp-file and replace behavior for settings and quick-label
  writes. Validate drafts before activating them. Keep cancel and failed-save
  behavior the same as the current settings workflow.
- When a settings change affects a running bridge or device service, use the
  existing restart and apply path and show the result to the user. Do not leave
  a service running on stale credentials or paths.

## Inventatory Scanner R1 security invariants

The scanner transport code is a security boundary:
`src/core/scanner/InventatoryScanProtocol.cpp` and
`src/platform/scanner/HttpServer.cpp`. Keep all of these true:

- BLE provisioning is physically verified. It provisions a random 32-byte
  shared secret. The desktop stores it in Credential Manager on Windows or
  Secret Service on Linux. The scanner stores it in NVS. It is never sent over
  HTTP.
- Requests authenticate the exact method, path, device identity, monotonic
  counter and exact JSON body with HMAC-SHA-256.
- The PC and the device use directional derived keys. Counters are monotonic
  and persist across restarts. Device-side reservations and PC-side accepted
  counters give replay resistance.
- The protocol protects integrity and freshness. It does not give payload
  confidentiality. Do not call it encrypted transport, and do not build it as
  one, without a deliberate protocol design and migration.
- mDNS is for discovery only. Do not weaken authentication, bind the service
  publicly, assume port forwarding, or remove the private-LAN and user-consent
  boundary.
- Token rotation, clear and reset, device identity changes, and replay-state
  changes stay coordinated and fail safely.

For protocol or replay changes, add positive and negative tests. Cover wrong
method, path, body and device, a bad MAC, stale or replayed counters, restart
persistence, rotation and reset, malformed JSON, and durable-callback failure.
Do not log tokens, MAC material, full authenticated bodies or credentials while
debugging.

See also [scanner-transport-security.md](scanner-transport-security.md).

## Vendor data and label units

Label text is built from supplier records (TME, DigiKey, and any vendor added
later). The same part must print the same way whichever vendor supplied it.
These rules apply to every vendor.

- Units are normalized once, in the shared label code, not in a vendor parser.
  A parser passes the vendor's value through with its unit. It never prints a
  scaled or base-unit form of its own. `normalizedUnitValue`
  (`src/label_printer/core/LabelPrinterDetails.cpp`) gives every quantity one
  form:
  - The figure has no space before its unit: `16V`, `600mV`, `1.5µH`, `26mΩ`.
  - The SI prefix is the one that leaves a mantissa from 1 to 999. So `0.25W`
    and `250 mW` both print `250mW`, and `0.6kV` prints `600V`.
  - Capacitance is the exception, because electrolytics are written in µF:
    F from 1 F, mF from 100 mF, µF below that, then nF and pF (`4700µF`,
    `100nF`).
  - A range has one prefix for both ends (`3.8 to 32V`, `100 to 600mV`).

  A value the rule does not own, such as `Ø5x11mm`, `±20%`, `2000h` or
  `-40 to 85°C`, prints as the vendor sent it.
- Package names: when two vendors name one package differently, add the pair to
  the alias table in `canonicalPackageName`, so both print one name. A lead
  style (`Radial`, `Axial`) is not a package. A cylinder prints its diameter
  (`Ø5mm`), because the vendors do not both give a body length.
- Manufacturer names: print the full name when it fits at the largest size.
  Otherwise cut it to its leading word (`Panasonic Industry` prints
  `Panasonic`) before printing any name in smaller type. The short brand names
  (ST, TI, ADI) are tried last.
- A new vendor is covered by adding its records to the parity fixture
  (`tests/fixtures/tme-digikey-parity.tsv`) and checking the difference list. A
  difference in units or package spelling is a rule to fix here. Only a real
  data difference, such as a different test point, may be exempted, and the
  exemption gives the reason.
