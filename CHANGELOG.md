# Changelog

All notable changes to the Inventatory software are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.1.1] - 2026-10-04

### Added
- Inventatory brand mark and pixel lockup integrated across the wizard flows and application shell.
- Physical value matching with localized unit parsing and part name comparison.

### Changed
- Refined rack compartment labels and updated default part label layout.
- Improved search performance and physical quantity matching.

### Fixed
- Resolved locale-sensitive character handling that could affect unit search on Linux systems.

## [v0.1.0] - 2026-09-15

### Added
- Initial public beta release of the Inventatory inventory management terminal application.
- Authoritative local SQLite inventory store for parts, racks, and movement history.
- Rack allocation, drawer visualizer, and compartment capacity tracking.
- CSV and structured file import workflows.
- Authenticated Inventatory Scan R1 hardware device pairing with HMAC-SHA-256 session integrity.
- Label printer discovery and direct label generation.
- Background services for Windows (system tray) and Linux (user systemd service).
