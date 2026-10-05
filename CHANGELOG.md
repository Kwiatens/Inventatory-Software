# Changelog

All notable changes to the Inventatory software are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.1.2] - 2026-10-05

### Fixed
- Launching Inventatory while its background service was busy or stuck no longer fails silently: the
  launch waits longer with visible progress, terminates a service that does not respond, and explains
  any failure instead of closing its window.
- The background service now cancels in-flight scanner requests and bounds its network workers when
  asked to quit, so it releases the workspace promptly.
- Windows: a background service whose notification-area controller cannot start now exits instead of
  running unreachable, no longer turns off the saved background preference, and re-adds its tray icon
  when Explorer restarts or starts late.
- Linux: closing the terminal window now saves the workspace and returns it to the background service.
- Linux: launching Inventatory while it is already open now says so and flags the open window.
- Linux: the background service is started through its systemd unit, the unit is rewritten only when
  it changes and keeps its registered executable, and signals are only sent to verified Inventatory
  processes.
- Linux: the desktop launcher and application icon are created reliably, the window keeps its
  dedicated identity and icon, and an unchanged launcher is no longer rewritten on startup.

### Changed
- The idle Linux background service uses far less CPU (event-driven signal handling and scanner
  event polling).

## [v0.1.1] - 2026-10-04

### Changed
- Refreshed the part and rack label designs.
- Refined rack compartment labels and updated default part label layout.
- Improved search performance and physical quantity matching.

### Fixed
- Resolved locale-sensitive character handling that could affect unit search on Linux systems.
- Fixed 'search by electrical quantities' function.

## [v0.1.0] - 2026-09-15

### Added
- Initial beta release start.
