# Public beta

Inventatory is a Windows-only public beta for local hardware inventory management.

Download the latest release package and run Install-Inventatory.cmd. The installer
downloads the package, verifies its SHA-256 checksum, installs for the current user,
and preserves existing Inventatory data during updates.

Build from a clean clone with Visual Studio 2022 C++ tools and CMake 3.20 or newer:

    cmake -S . -B build
    cmake --build build --config Release --target inventatory inventatory_tests -- /m:1
    ctest --test-dir build -C Release --output-on-failure

Inventory data defaults to Documents\\Inventatory and machine-local settings use
%LOCALAPPDATA%\\Inventatory. Scan R1 firmware using transport v3 must be re-paired
after update; open Settings > Scan R1 in the application to pair the scanner and
review its connection status.
