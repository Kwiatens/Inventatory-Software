# Native Linux support

Inventatory provides separate Windows and Linux builds from one shared C++17
application. The Linux release targets Ubuntu 24.04 LTS on x86-64. It keeps the
same FTXUI terminal interface, SQLite inventory and history, import/BOM
workflows, backup format, and Scan R1 protocol as Windows.

## Linux build

Install the Ubuntu 24.04 build dependencies:

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libsecret-1-dev \
  libcurl4-openssl-dev libssl-dev libglib2.0-dev
```

Configure, build, and run the automated tests in both configurations:

```sh
cmake -S . -B build-linux-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug --parallel
ctest --test-dir build-linux-debug --output-on-failure

cmake -S . -B build-linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release --parallel
ctest --test-dir build-linux-release --output-on-failure
```

The project builds SQLite and FTXUI from pinned source revisions. Its Linux
executable links to OpenSSL, libcurl, libsecret, and GIO. Install locally and
run it from a terminal with:

```sh
cmake --install build-linux-release --prefix "$HOME/.local"
"$HOME/.local/bin/inventatory"
```

In-app release updates require a build configured with
`-DINVENTATORY_RELEASE_REPOSITORY=owner/repository`, as in the CI release job.
The updater verifies the archive and installer checksums before replacing a
user-writable executable.

## Runtime integrations

The required shared libraries on Ubuntu 24.04 are provided by
`libsecret-1-0`, `libcurl4`, `libssl3`, and `libglib2.0-0`. Secret storage also
requires a running Secret Service provider, such as GNOME Keyring. If the
service is unavailable or locked, secret reads and writes fail closed; scanner
tokens and DigiKey secrets are never written to ordinary configuration files.

Optional desktop and hardware packages enable their corresponding existing
workflows:

- `bluez` and an available Bluetooth adapter for first-time Scan R1 BLE
  provisioning. The user must enter the six-digit code shown by the scanner;
  the Linux BlueZ agent accepts the pairing only for the selected
  Inventatory service and verified code.
- `avahi-daemon` for private-LAN mDNS scanner discovery. Inventatory registers
  `_inventatory._tcp` directly with the running daemon over the system D-Bus,
  bound to the one interface that owns the scanner service's private IPv4
  address (never other interfaces); `avahi-utils` is not required. The daemon
  is not started on demand: if it is not running, the service still works and
  the app reports that network discovery is unavailable. The advertisement ends
  when the application stops or exits.
- `cups-client` and a configured CUPS queue for printer discovery and raw ZPL
  label output. Configure the queue for a ZPL-compatible printer.
- `zenity` for native file and folder chooser dialogs.
- `xdg-utils` for opening HTTPS links; `wl-clipboard` on Wayland, or `xclip` or
  `xsel` on X11, for clipboard output.

Scan R1 data sync continues to use the existing authenticated HTTP protocol
over the private LAN. Authentication, device identity, monotonic replay
counters, acknowledgments, and offline queue behavior remain unchanged.
Protocol authentication provides integrity and replay protection, not payload
encryption. Do not expose the listener through public networks or port
forwarding. BLE provisioning requires BlueZ's system D-Bus service and an
interactive user session. There is no desktop serial or USB scanner transport
in the existing application on either platform.

The Linux background option uses a per-user systemd unit and a private runtime
lock. Launching Inventatory while its background scanner service is active
asks the service to save and quit, waits up to 12 seconds (showing progress),
terminates it if it still has not released the workspace, and then opens the
terminal UI. If the service cannot be stopped at all, the launch explains why
and leaves the workspace untouched. Launching while a terminal UI is already
open rings the bell in that window and tells the new window to close.

When the UI exits normally or its terminal window is closed, the workspace is
saved and the systemd unit is started again so the service stays supervised
(`systemctl --user status inventatory-background.service`); a detached process
is used only if systemd refuses. The unit is written only when its content
changes and keeps the executable it was registered with while that file still
exists. A binary that was replaced on disk while it runs (an update or
reinstall) is never registered as `... (deleted)`. The single-instance locks
live in `$XDG_RUNTIME_DIR/inventatory`, or in `/run/user/<uid>/inventatory`
when the variable is not set (for example from cron or `su`), so every launch
context shares them. Linux does not provide a notification area icon. The current Windows
release retains its notification-area process and Windows startup integration.

## Data and configuration

Linux follows XDG locations when the corresponding environment variable is an
absolute path:

- Settings: `${XDG_CONFIG_HOME:-$HOME/.config}/Inventatory/settings.conf`.
- Default inventory workspace: `${XDG_DATA_HOME:-$HOME/.local/share}/Inventatory`.
- Update status and release notes: the settings directory above.
- Background unit: `${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/inventatory-background.service`.

The workspace contains `inventory.db` and, when used, `activity.tsv`,
`printer.conf`, `quick_labels.conf`, and `inventatory_scan.conf`. A pre-existing
`$HOME/Documents/Inventatory/inventory.db` is recognized if the new XDG default
does not already contain a database; Inventatory does not move it. The selected
workspace can also be changed in Settings. Keep the Windows settings file
separate from Linux settings; backup bundles are the supported way to transfer
an inventory workspace between systems. Backups sanitize the machine-specific
workspace path, preserve the existing SQLite schema and history, and exclude
credentials and scanner pairing state. Restore validates the bundle and creates
a pre-restore backup before activation.

Settings, `printer.conf`, quick labels and the update marker are written
through a temporary file that is flushed to disk, then renamed over the
original, and the directory is flushed afterwards. A settings file that is a
symbolic link (for example one managed by a dotfile tool) stays a link; the
file it points to is updated. An unchanged `printer.conf` is not rewritten, and
one that exists but cannot be read is never replaced by an empty configuration.

Paths with spaces and UTF-8 names are supported. Linux filesystems are treated
as case-sensitive. The database, inventory records, backups, CSV data, and
scanner protocol remain compatible with Windows. Secrets are stored in each
operating system's native credential service and are not transferred with the
inventory.

## Architecture and validation

CMake selects platform implementations for terminal helpers, startup and
background integration, credentials, HTTPS, printing, and BLE. Inventory
domain logic, FTXUI pages, SQLite schema and migrations, backup/restore
validation, and scanner protocol handling remain shared. Windows builds still
select the Windows SDK, Credential Manager, WinHTTP, print spooler, DNS-SD,
BLE, and updater implementations; Linux-specific libraries are linked only by
Linux targets.

The release workflow builds and tests both Debug and Release Linux targets on
Ubuntu 24.04, packages the Linux application with its own installer and
`SHA256SUMS-linux.txt`, then publishes those beside the unchanged Windows
assets. The Linux and Windows update services each accept only their own
release asset names.

Native TUI rendering, file dialogs, Bluetooth pairing, CUPS printing, mDNS
discovery, and Windows-to-Linux/Linux-to-Windows restore should be exercised on
their respective systems. CI can verify Linux compilation and automated core
tests, but it has no physical Scan R1 device, Bluetooth adapter, or label
printer. See the release and CI results for what has actually run; compilation
alone is not hardware or terminal acceptance.
