<div align="center">

<a href="https://kwiatens.github.io/Inventatory-Site/">
  <img src="branding/inventatory-lockup.svg" alt="Inventatory" width="560">
</a>

**Open-source, terminal-based hardware inventory management for PCB designers.**

Track your hardware components in a fast terminal UI. Designed so that most of the work is automated.
Features a 3D printable rack system, label printing, the 'Inventatory Scanner R1' device and much more.

[![Release](https://img.shields.io/github/v/release/Kwiatens/Inventatory-Software?include_prereleases&style=flat-square&color=58B9B0&labelColor=0D1010)](https://github.com/Kwiatens/Inventatory-Software/releases/latest)
[![License](https://img.shields.io/github/license/Kwiatens/Inventatory-Software?style=flat-square&color=58B9B0&labelColor=0D1010)](LICENSE)
![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-58B9B0?style=flat-square&labelColor=0D1010)
![C++17](https://img.shields.io/badge/C%2B%2B-17-58B9B0?style=flat-square&labelColor=0D1010)

[**Website**](https://kwiatens.github.io/Inventatory-Site/) &nbsp;·&nbsp;
[**Scanner Firmware**](https://github.com/Kwiatens/Inventatory-Firmware) &nbsp;·&nbsp;
[Documentation](https://kwiatens.github.io/Inventatory-Site/docs/) &nbsp;·&nbsp;
[Report an issue](https://github.com/Kwiatens/Inventatory-Software/issues)

</div>

---
> **New here?** The [Inventatory website](https://kwiatens.github.io/Inventatory-Site/) is a great place to start.
> There you can explore what the project can do and find the documentation and instructions.

> **Current version:** public beta (`0.1.6`, see [CHANGELOG.md](CHANGELOG.md)).

## Install on Windows

Open Command Prompt and paste the following command:

    curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.ps1" -o "%TEMP%\Install-Inventatory.ps1" && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\Install-Inventatory.ps1" && del /q "%TEMP%\Install-Inventatory.ps1"

## Install on Linux

Any x86-64 Linux distribution with glibc 2.34 or newer should work. The release is tested on Ubuntu 22.04,
Debian 12, AlmaLinux 9 (a RHEL 9 rebuild) and Fedora 44. It also needs OpenSSL 3, libsecret, libcurl and a
Secret Service provider (GNOME Keyring or KWallet) to store tokens.

    mkdir -p "$HOME/.cache/inventatory-installer" && cd "$HOME/.cache/inventatory-installer" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-linux-x64.tar.gz" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/SHA256SUMS-linux.txt" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.sh" && chmod +x Install-Inventatory.sh && ./Install-Inventatory.sh

Then run with
`$HOME/.local/bin/inventatory`.

<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/roadmap-light.svg">
  <img src="branding/readme/roadmap.svg" alt="Plans and future features: MCP integration, multi-user workflow, more vendor APIs, more EDA software, custom rack sizes and parametric racks." width="830">
</picture>

## Building and testing

All the information here and below is only useful, when you want to manually compile the software.

Build with CMake 3.20 or newer and a C++17 compiler, then run tests with `ctest --test-dir <build-dir> --output-on-failure`.

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md): how to contribute.
- [SECURITY.md](SECURITY.md): private vulnerability reporting and the security scope.
- [AGENTS.md](AGENTS.md): architecture, invariants and verification rules, also the guide for coding agents.
- [docs/project-standards.md](docs/project-standards.md): rules shared with the Scan R1 firmware repository.
- [docs/releasing.md](docs/releasing.md): release steps and repository settings.
- [CHANGELOG.md](CHANGELOG.md): what changed in each version.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): licenses of bundled dependencies.

## License

Inventatory is licensed under GPL-3.0-only. See [LICENSE.md](LICENSE.md) for the project notice.
