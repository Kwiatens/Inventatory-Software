# Public beta

Inventatory is a public beta for local hardware inventory management with
separate native Windows and Linux x86-64 releases. The Windows release and its
installation workflow remain supported.

For a new installation, open Command Prompt and paste this single command:

    curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.ps1" -o "%TEMP%\Install-Inventatory.ps1" && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\Install-Inventatory.ps1" && del /q "%TEMP%\Install-Inventatory.ps1"

The installer verifies the downloaded release archive against its SHA-256
manifest before activation, then launches Inventatory.
The first-run setup wizard creates the desktop shortcut after setup is
completed.
Use Settings > Updates for later updates so the existing verified update and
rollback path is used.
After installation, Settings > Updates can check GitHub and open the same
verified release package through an in-app update wizard. The wizard previews
the bounded release notes, offers to save pending Settings changes, shows
download progress/speed/ETA, verifies both the package and installer, and then
restarts Inventatory through the user's configured terminal application.

The in-app download can be cancelled while the package is downloading. Checksum
verification and installer handoff then complete without interruption. GitHub
prerelease tags such as `v0.2.0-rc.1` are also accepted by the updater so
maintainers can test the complete route before a stable release. A failed download
or verification returns to a retryable error screen without changing the active
installation. If replacement fails after Inventatory closes, the installer
rolls back the old installation, writes a machine-local completion result under
%LOCALAPPDATA%\Inventatory, and relaunches the previous version. Inventory data,
settings, secrets, and release notes are never stored in the inventory database.

For Ubuntu 24.04 LTS, download the Linux release assets into one directory and
run the checksum-verifying installer:

    mkdir -p "$HOME/.cache/inventatory-installer" && cd "$HOME/.cache/inventatory-installer" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-linux-x64.tar.gz" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/SHA256SUMS-linux.txt" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.sh" && chmod +x Install-Inventatory.sh && ./Install-Inventatory.sh

This installs the native executable to `~/.local/bin/inventatory`. The Linux
build shares the inventory, history, backup, import, BOM, label, and scanner
protocol code. Runtime packages and scanner setup are listed in
[Linux support](linux-support.md).

Build from a clean clone with Visual Studio 2022 C++ tools and CMake 3.20 or newer:

    cmake -S . -B build -DINVENTATORY_RELEASE_REPOSITORY=Kwiatens/Inventatory-Software -DINVENTATORY_SCAN_FIRMWARE_REPOSITORY=Kwiatens/Inventatory-Hardware
    cmake --build build --config Release --target inventatory inventatory_background inventatory_tests inventatory_input_tests -- /m:1
    ctest --test-dir build -C Release --output-on-failure

Build Linux natively on Ubuntu 24.04 after installing the dependencies listed
in [Linux support](linux-support.md):

    cmake -S . -B build-linux-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-linux-debug --parallel
    ctest --test-dir build-linux-debug --output-on-failure
    cmake -S . -B build-linux-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-linux-release --parallel
    ctest --test-dir build-linux-release --output-on-failure
    cmake --install build-linux-release --prefix "$HOME/.local"
    "$HOME/.local/bin/inventatory"

Maintainers can exercise the isolated installer update transaction without
touching a real installation with:

    pwsh -NoProfile -File installer\Test-InventatoryUpdate.ps1

On Windows, inventory data defaults to Documents\\Inventatory and machine-local
settings use %LOCALAPPDATA%\\Inventatory. On Linux, the default workspace uses
`$XDG_DATA_HOME/Inventatory` (or `~/.local/share/Inventatory`) and settings use
`$XDG_CONFIG_HOME/Inventatory` (or `~/.config/Inventatory`). Open Settings >
Scan R1 in the application to pair the scanner and review its connection status.
