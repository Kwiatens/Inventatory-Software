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

## Public beta

Inventatory is an early Windows-only public beta. Build it with CMake and Visual Studio 2022, or download the current release package and run `Install-Inventatory.cmd`. See the [public-beta guide](docs/public-beta.md) for install, build, test, data-location, and Scan R1 setup guidance.

The application stores normal inventory data under `Documents\\Inventatory` and machine-local settings under `%LOCALAPPDATA%\\Inventatory`.

## License

Inventatory is licensed under GPL-3.0-only. Each release package includes the complete license text; see [LICENSE.md](LICENSE.md) for the project notice.
