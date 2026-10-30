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
[Scanner Firmware](https://github.com/Kwiatens/Inventatory-Firmware) &nbsp;·&nbsp;
[Documentation](https://kwiatens.github.io/Inventatory-Site/docs/) &nbsp;·&nbsp;
[Report an issue](https://github.com/Kwiatens/Inventatory-Software/issues)

</div>

> **New here?** Visit the [Inventatory website](https://kwiatens.github.io/Inventatory-Site/) - it's a great place to start.
> You can explore what the project can do and find the documentation and instructions there.


## Core features of the system


<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/media/intake-light.gif">
  <img src="branding/readme/media/intake.gif" align="right" width="278" alt="The scanner reads the code on a part's bag, the label printer feeds out the new label, the label goes onto a tube, and the tube goes into its free slot, R1-B3.">
</picture>

### 1. Automation is the priority

Everything inside Inventatory was designed to be automated and operate quickly as possible. 
Your job is designing and working on your projects, not baby-sit your inventory. It defeats the whole purpose as it's supposed to save you time.

It automatically:
- Assigns every single parameter (name, type, electrical parameters, datasheet links, pricing and purchase link, etc) of the part.
- Matches parts to your PCB BOM from KiCad, EasyEDA or Altium Designer, based on the electrical parameters. When a part is not in your inventory, it will search for a part that matches what you need for the best price, from your preferred supplier.
- Puts your parts on available racks and slots in that racks, for the exact part positioning.
- Generates and prints the label that goes on each storage tube, with all the information about a part.
- Creates shopping lists that you can put directly to your vendor.
- And much more!

<br clear="right">


<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/media/place-light.gif">
  <img src="branding/readme/media/place.gif" align="right" width="278" alt="The camera tilts up over the rack and its grid is named: columns A to E, rows 1 to 5. Back in the normal view, the cursor goes to slots R1-B3, R1-C1 and R1-C2 one by one, and each tube comes up out of the rack and goes back in.">
</picture>

### 2. Each part gets assigned to it's own unique storage location

Instead of manually assigning every single component to a random box, which doesn't really solve anything - Inventatory uses our 3D-printable storage rack system. 
Racks allow to have a unique location for each and every part, meaning the software can tell you where your parts are **precisely**.

This means that when you then upload your BOM (Bill of materials) of your PCB - it will give you not only if you have the needed parts, but also on **which exact rack and slot they are at**, so you can just grab the highlighted tubes and get going.

<br clear="right">


<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/media/search-light.gif">
  <img src="branding/readme/media/search.gif" align="right" width="278" alt="Three searches, one after another: 100nF, 4.7kΩ and 2.2µF. Each one lights two tubes in the rack, one labelled the way it was typed and one labelled another way: 0.1µF, 4k7 and 2u2.">
</picture>

### 3. Search by electrical parameters, not only part names

A common problem with inventory systems is that they are not purpose build for electronics. You might type 100nF in the search function, and nothing will pop-up, even though the part is actually there, just marked as 0.1uF. Inventatory solves this, by searching for parts by their physical electrical parameters, not just names.

<br clear="right">


<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/media/history-light.gif">
  <img src="branding/readme/media/history.gif" align="right" width="278" alt="Three tubes with their counts: 100, 80 and 50. Parts are used three times; each time a tube's chips sink, its count drops and a new commit is added to the line above. Then the cursor steps back along the line to the first commit, and the tubes fill back up to 100, 80 and 50.">
</picture>

### 4. It keeps you changes in check, just like git

Every change is saved as a commit with its before and after values, like a git history for your parts. You can
see exactly what changed, set checkpoints, restore a whole snapshot, or revert a single change while keeping
everything since.

<br clear="right">


## We have a purpose-built, DiY-able scanner too!

<picture>
  <source media="(prefers-color-scheme: light)" srcset="branding/readme/media/scanner-light.gif">
  <img src="branding/readme/media/scanner.gif" align="right" width="278" alt="The QR code on a capacitor's label is scanned. On the scanner's screen the part comes up, 1 and 2 are typed, and B removes 12: the screen says Removed 12 / total 38.">
</picture>

It's designed specyfically for this system. It allows you to modify the quantity of a part you're actively working with, without having to fiddle around with your PC during PCB work, which from experience is very frustrating.

It's main purpose is allowing you to just scan a QR code on the generated label that's on the part container, and then deduct or add quantity to that exact part with just two clicks on the scanner.

You can also use it to scan vendor bags or send jobs to your label printer.

> **[Scanner firmware and build guide →](https://github.com/Kwiatens/Inventatory-Firmware)**

Inventatory works fully without the scanner. It just makes the intake faster.

<br clear="right">

## Installation

### Windows

Open CMD and paste:

    curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.ps1" -o "%TEMP%\Install-Inventatory.ps1" && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\Install-Inventatory.ps1" && del /q "%TEMP%\Install-Inventatory.ps1"

### Linux

    mkdir -p "$HOME/.cache/inventatory-installer" && cd "$HOME/.cache/inventatory-installer" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-linux-x64.tar.gz" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/SHA256SUMS-linux.txt" && curl -fLO "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.sh" && chmod +x Install-Inventatory.sh && ./Install-Inventatory.sh

## Build
Build with CMake 3.20 or newer.

Ryn the tests with `ctest --test-dir <build-dir> --output-on-failure`.

## License

Inventatory is licensed under GPL-3.0-only. See [LICENSE.md](LICENSE.md) for the project notice.
