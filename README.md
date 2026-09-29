<div align="center">

<a href="https://kwiatens.github.io/Inventatory-Site/">
  <img src="branding/inventatory-lockup.svg" alt="Inventatory" width="560">
</a>

**Open-source, terminal-based hardware inventory management for PCB designers.**

Track your hardware components in a fast terminal UI. The system is designed, so that most of the work is automated.
Features a 3D printable rack system, label printing, the 'Inventatory Scan R1' barcode scanner and much more.

[![Release](https://img.shields.io/github/v/release/Kwiatens/Inventatory-Software?include_prereleases&style=flat-square&color=58B9B0&labelColor=0D1010)](https://github.com/Kwiatens/Inventatory-Software/releases/latest)
[![License](https://img.shields.io/github/license/Kwiatens/Inventatory-Software?style=flat-square&color=58B9B0&labelColor=0D1010)](LICENSE)
![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-58B9B0?style=flat-square&labelColor=0D1010)
![C++17](https://img.shields.io/badge/C%2B%2B-17-58B9B0?style=flat-square&labelColor=0D1010)

[**Website**](https://kwiatens.github.io/Inventatory-Site/) &nbsp;·&nbsp;
[Documentation]([docs/user-guide.md](https://kwiatens.github.io/Inventatory-Site/docs/)) &nbsp;·&nbsp;
[Install](https://kwiatens.github.io/Inventatory-Site/#homepage-download) &nbsp;·&nbsp;
[Report an issue](https://github.com/Kwiatens/Inventatory-Software/issues)

</div>

> **New here?** The [Inventatory website](https://kwiatens.github.io/Inventatory-Site/) is a great place to start.
> There you can explore what the project can do, and is where you can find the documentation and instructions.

---

## Install on Windows

Open Command Prompt and paste the following command to download and install the
latest stable release for the current Windows user:

    curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.ps1" -o "%TEMP%\Install-Inventatory.ps1" && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\Install-Inventatory.ps1" && del /q "%TEMP%\Install-Inventatory.ps1"

## Install on Linux

On Ubuntu 24.04:

    mkdir -p "$HOME/.cache/inventatory-installer" && cd "$HOME/.cache/inventatory-installer" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-linux-x64.tar.gz" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/SHA256SUMS-linux.txt" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.sh" && chmod +x Install-Inventatory.sh && ./Install-Inventatory.sh

Run it with
`$HOME/.local/bin/inventatory`. See [Linux support](docs/linux-support.md) for
runtime dependencies, scanner access, data locations, and native build steps.

The whole system is **designed to be extremely fast, and requires as little user input as possible.**

Key features:
- The system automatically assigns each part to a specific slot on each 3D-printable rack, so later it can give you the precise location of it, not just generic 'Drawer 12'.
- Integration with popular EDA software - it automatically compares your inventory with the BOM of your PCB, and points out precisely on which rack and slot each component lives.
- DIY-able Hardware scanning device called 'Inventatory Scanner' - used for scanning vendor part bags, and Inventatory QR codes from the SMD tubes on the racks.
- Direct integration with the most popular electronics part vendors (like DigiKey).
- ZPL Label Printer integration for the tubes that go on 3D printable storage racks.
- Search by electrical parameters, not only by name! Electrical parameters are applied automatically too, from the vendor's API.

## Plans and future features
- MCP Integration
- Multi-user workflow
- Integration with more vendor APIs
- Expand on the supported EDA software
- Support for custom rack sizes
- Parametric racks

## License

Inventatory is licensed under GPL-3.0-only. Each release package includes the complete license text; see [LICENSE.md](LICENSE.md) for the project notice.
