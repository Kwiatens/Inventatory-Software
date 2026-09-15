# Inventatory-Software

Inventatory is an open-source, lightweight, terminal-based hardware inventory tracking system.
It is purpose-built for PCB designers, but is also great for keeping track of regular hardware - like screws/fasteners etc.

## Install on Windows

Open Command Prompt and paste the following command to download and install the
latest stable release for the current Windows user:

    (if not exist "%LOCALAPPDATA%\Programs" mkdir "%LOCALAPPDATA%\Programs") && curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-win-x64.zip" -o "%TEMP%\Inventatory-win-x64.zip" && tar.exe -xf "%TEMP%\Inventatory-win-x64.zip" -C "%LOCALAPPDATA%\Programs" && del /q "%TEMP%\Inventatory-win-x64.zip" && "%LOCALAPPDATA%\Programs\Inventatory\inventatory.exe"

The command uses only tools included with supported Windows versions. It
extracts the release directly into the per-user program directory and launches
Inventatory in the same terminal. The first-run setup wizard creates the
desktop shortcut after setup is completed.

The whole system is **designed to be extremely fast, and requires as little user input as possible.**

Inventatory system key features:
- Automatic tracking of SMD components quantity and their exact physical in a 3D printable storage rack solution.
- Direct integration with the most popular electronics part vendors (Like DigiKey).
- DIY-able Hardware scanning device called 'Inventascan' - used for scanning vendor part bags, and Inventatory QR codes from the SMD tubes on the racks.
- Integration with popular PCB CAD (KiCad) - automatically compares your inventory with the BOM of your PCB, points out at what rack and exact rack slot each component lives.
- ZPL Label Printer integration for the tubes that go on 3D printable storage racks.
- Search by electrical parameters, not only by name! Electrical parameters are applied automatically too, from the vendor's API :)

## A bit about the project
The Inventatory system is a side-project of mine. I've built this because I needed to find a way to organize my own SMD components - after a bunch of projects over the years it was a nightmare to find anything, so I ended up just reordering parts that I already had, I'm not even gonna mention how much time got wasted.

My goal was to create a very automated, fast and 'function over form' system. It's purpose should be to take work off my hands and let me focus on building my projects, instead of manually micro-managing my inventory. 
I didn't want another distraction from the already challenging problem solving required in debugging circuits.
Key feature was that it had to track the physical location of the SMD components, and not use generic "Cardboard Box Number 8" categorization.

Key assumptions during development were that it needs to run in a simple input/output with minimal clutter. This is why I went with a TUI-based approach, not web-based as most similar projects do.

I came up with a 3D printable rack system, it uses cheap and easy to find plastic tubes that you can buy for cheap (200pcs for ~10 euro), and automatically generated labels that contain the most important information + a custom ID QR code that will be used with my custom 'Inventascan R1' hardware scanning device. Speaking of which was also a key feature. Scanning with web/phone cameras works, but again it's clunky.
I'd rather have a purpose-made device that does one thing it was designed for - scan 2D codes.

I plan on continuously developing it, and adding new features. I'm the user myself :)
Just as I said, keep in mind that it's a side project for me - not a main one.

I would be happy to see other people contribute to it

## Plans and future features
- Multi-user workflow
- MCP Integration
- Integration with more vendor APIs
- Expand on the supported PCB CAD software.

## License

Inventatory is licensed under GPL-3.0-only. Each release package includes the complete license text; see [LICENSE.md](LICENSE.md) for the project notice.
