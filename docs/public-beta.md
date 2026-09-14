# Public beta

Inventatory is a Windows-only public beta for local hardware inventory management.

Download the latest release package and run Install-Inventatory.cmd for a new
installation. The installer verifies the published SHA-256 manifest, installs
for the current user, and preserves existing Inventatory data during updates.
After installation, Settings > Updates can check GitHub and open the same
verified release package through an in-app update wizard. The wizard previews
the bounded release notes, offers to save pending Settings changes, shows
download progress/speed/ETA, verifies both the package and installer, and then
restarts Inventatory through the installer.

The in-app download can be cancelled until installer handoff. GitHub prerelease
tags such as `v0.2.0-rc.1` are also accepted by the updater so maintainers can
test the complete route before a stable release. A failed download
or verification returns to a retryable error screen without changing the active
installation. If replacement fails after Inventatory closes, the installer
rolls back the old installation, writes a machine-local completion result under
%LOCALAPPDATA%\Inventatory, and relaunches the previous version. Inventory data,
settings, secrets, and release notes are never stored in the inventory database.

Build from a clean clone with Visual Studio 2022 C++ tools and CMake 3.20 or newer:

    cmake -S . -B build
    cmake --build build --config Release --target inventatory inventatory_background inventatory_tests -- /m:1
    ctest --test-dir build -C Release --output-on-failure

Maintainers can exercise the isolated installer update transaction without
touching a real installation with:

    pwsh -NoProfile -File installer\Test-InventatoryUpdate.ps1

Inventory data defaults to Documents\\Inventatory and machine-local settings use
%LOCALAPPDATA%\\Inventatory. Open Settings > Scan R1 in the application to pair
the scanner and review its connection status.
