# Inventatory-Software

Inventatory is a Windows terminal application for local hardware inventory management and Scan R1 integration.

For the recommended Windows Terminal presentation, add the profile from
[`docs/windows-terminal-profile.json`](docs/windows-terminal-profile.json) as an
`Inventatory` profile, then use `.\run.ps1 -WindowsTerminal`. The normal
`.\run.ps1` launch remains available and does not require Windows Terminal or
JetBrains Mono.

## Public beta

Inventatory is an early Windows-only public beta. Build it with CMake and Visual Studio 2022, or download the current release package and run `Install-Inventatory.cmd`. See the [public-beta guide](docs/public-beta.md) for install, build, test, data-location, and Scan R1 setup guidance.

The application stores normal inventory data under `Documents\\Inventatory` and machine-local settings under `%LOCALAPPDATA%\\Inventatory`.

## License

Inventatory is licensed under GPL-3.0-only. Each release package includes the complete license text; see [LICENSE.md](LICENSE.md) for the project notice.
