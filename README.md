# Inventatory-Software

Inventatory is a open-source, lightweight, terminal based hardware inventory tracking system.
It is purpose build for PCB designers, but is also great for keeping track of regular hardware - like screws/fasteners etc.

The whole system is **designed to be extreamly fast, and require as little user input as possible.**

Inventatory system key features:
- Automatic tracking of SMD components quantity and their exact physical in a 3D printable storage rack solution.
- Direct integration with the most popular electronics part vendors (Like DigiKey).
- DiY-able Hardware scanning device called 'Inventascan' - used for scanning vendor part bags, and Inventatory QR codes from the SMD tubes on the racks.
- Integration with popular PCB CAD (KiCAD) - automatically compares your inventory with the BOM of your PCB, points out at what rack and exact rack slot each component lives.
- ZPL Label Printer integration for the tubes that go on 3D printable storage racks.
- Search by electrical parameters, not only by name! Electrical parameters are applied automatically too, from the vendor's API :)

## A bit about the project
The Inventatory system is a side-project of mine. I've build this because I needed to find a way to organize my own SMD components - after a bunch of projects over the years it was a nightmare to find anything, so I ended up just reordering parts that I already had, I'm not even gonna mention how much time got wasted.

My goal was to create a very automated, fast and 'function over form' system. It's purpose should be to take work off my hands and let me focus on building my projects, instead of manually micro-managing my inventory. 
I didn't want another distraction from the already challenging problem solving required in debugging circuits.
Key feature was that it had to track the phisical location of the SMD components, and not use generic "Cardboard Box Number 8" categorization.

Key assumptions during development was that it needs to run in a simple input/output with minimal clutter. This is why I went with a TUI based approach, not web based as most simillar projects do.

I came up with a 3D printable rack system, it uses cheap and easy to find plastic tubes that you can buy for cheap (200pcs for ~10 euro), and automatically generated labels that contain the most important information + a custom ID QR code that will be used with my custom 'Inventascan R1' hardware scanning device. Speaking of which was also a key feature. Scanning with web/phone cameras works, but again it's clunky.
I'd rather have a purpose made device that does one thing it was designed for - scan 2D codes.

I plan on continuesly developing it, and adding new features. I'm the user myself :) 
Just as I said, keep in mind that it's a side project for me - not a main one.

I would be happy to see other people contribute to it

## Plans and future features
- Multi-user workflow
- MCP Integration
- Integration with more vendor API's
- Expand on the supported PCB CAD software.

## Public beta

Inventatory is an early Windows-only public beta. Build it with CMake and Visual Studio 2022, or download the current release package and run `Install-Inventatory.cmd`. See the [public-beta guide](docs/public-beta.md) for install, build, test, data-location, and Scan R1 setup guidance.

The application stores normal inventory data under `Documents\\Inventatory` and machine-local settings under `%LOCALAPPDATA%\\Inventatory`.

## License

Inventatory is licensed under GPL-3.0-only. Each release package includes the complete license text; see [LICENSE.md](LICENSE.md) for the project notice.
