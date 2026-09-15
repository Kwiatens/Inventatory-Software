# Public beta

Inventatory is a Windows-only public beta for local hardware inventory management.

For a new installation, open Command Prompt and paste this single command:

    (if not exist "%LOCALAPPDATA%\Programs" mkdir "%LOCALAPPDATA%\Programs") && curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-win-x64.zip" -o "%TEMP%\Inventatory-win-x64.zip" && tar.exe -xf "%TEMP%\Inventatory-win-x64.zip" -C "%LOCALAPPDATA%\Programs" && del /q "%TEMP%\Inventatory-win-x64.zip" && "%LOCALAPPDATA%\Programs\Inventatory\inventatory.exe"

This uses only tools included with supported Windows versions. It downloads the
latest stable release ZIP directly from GitHub, extracts it into the current
user's program directory, and launches Inventatory in the same terminal. The
first-run setup wizard creates the desktop shortcut after setup is completed.
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

Build from a clean clone with Visual Studio 2022 C++ tools and CMake 3.20 or newer:

    cmake -S . -B build -DINVENTATORY_RELEASE_REPOSITORY=Kwiatens/Inventatory-Software -DINVENTATORY_SCAN_FIRMWARE_REPOSITORY=Kwiatens/Inventatory-Hardware
    cmake --build build --config Release --target inventatory inventatory_background inventatory_tests -- /m:1
    ctest --test-dir build -C Release --output-on-failure

Maintainers can exercise the isolated installer update transaction without
touching a real installation with:

    pwsh -NoProfile -File installer\Test-InventatoryUpdate.ps1

Inventory data defaults to Documents\\Inventatory and machine-local settings use
%LOCALAPPDATA%\\Inventatory. Open Settings > Scan R1 in the application to pair
the scanner and review its connection status.
