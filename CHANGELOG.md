# Changelog

All notable changes to the Inventatory software are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Stock: Sort / Filter is a panel under the list header that stays open while you choose. Each
  choice applies at once, and the panel shows how many parts match. The header shows the sort and
  the modification-date filter in use, so the list's state is visible with the panel closed.

### Removed
- Stock: the number keys 1 to 5 no longer pick filter options. Use the arrow keys and Enter, or click.

### Fixed
- Projects: when saving the stock taken for a build fails, the build no longer stays open, so Deduct
  from stock cannot be confirmed a second time and take the same parts out twice. Press R to retry the save.
- Racks: Print, Enter and Detail on a rack slot now act on the part in that slot, whatever the Stock
  list is sorted or filtered by. A part added in Stock is selected after saving.
- DigiKey: a lookup that finds no exact match no longer fills the part with a similar-looking product.
  Control characters in DigiKey text are shown as spaces, and text with an embedded NUL is refused.

## [v0.1.4] - 2026-10-08

### Added
- Projects: Pick parts walks the BOM one rack at a time. Each rack's grid lights the slots to open with
  the value and the amount to take, the pick list gives the board references for each slot, and the
  route line shows the racks still to visit and Finish.
- Projects: parts kept outside racks are collected in one final stop, grouped by location.
- Projects: the finish screen lists every part taken, where it came from and the stock it leaves, with
  Deduct from stock and Keep stock. Low and out-of-stock results are marked in words.
- Settings: Quick Labels is listed under Printer, and every category uses the same row layout. Setting
  names say what they do ("Keep running when closed", "Label after each scan", "Rack symbols").
- The terminal UI style guide (`docs/ui-style-guide.md`) sets the rules for glyphs, alignment, controls,
  colour, status, and plain-sentence page headers. `AGENTS.md` links to it.

### Changed
- Projects: Find in racks is called Pick parts. A BOM comparison splits into In stock and Missing, with
  Need, Have, and Status (Ready, Short N, or Missing) columns and a Shopping list action.
- Projects: confirming a rack is one action. Slots no longer need to be confirmed one by one.
- Projects: a partly stocked line picks what is available, and a build that is not fully covered can
  still be deducted, so the stock on hand is reduced by exactly what was taken.
- Settings: each category has a single row model with a row cursor, aligned labels, values and
  buttons, and a header that states the category's state. Unsaved changes show their saved value
  beside the new one, and Save and Discard appear in the header while changes are pending.
- Colours: the status palette moves to the calmer Slate set (sea glass for ready, sand for warning,
  muted rose for danger) so it matches the petrol-cyan base. Installs that kept the old default colours
  move to the new defaults; customised colours are kept.
- Text: decorative glyphs (dots, arrows, spinners, progress bars, bracketed key hints, and `[i]`/`[!]`
  prefixes) are replaced with plain words and ASCII characters.

### Removed
- The per-slot confirmation of Find in racks and the unused spinner and progress-bar helpers.

### Fixed
- Projects: parts with no stock are no longer listed as picks, counted in pick totals, or used to block
  the walkthrough while other parts are short.
- Settings: moving the cursor or the mouse wheel on the Printer page no longer changes the configured
  printer, and Esc asks before discarding unsaved changes.
- Value search ("Closest to" and plain values such as `100nF`) no longer lists parts that are not the
  same kind of component (switches, MOSFETs, resistors) or whose value is more than 33% away from the
  target; only Exact, Workable (within 10%) and Possible (within 33%) matches are shown.

## [v0.1.3] - 2026-10-06

### Security
- Scan R1: the copied pairing token is kept out of Windows clipboard history and cloud clipboard sync,
  and is flagged sensitive for `wl-copy` on Linux.
- Restore, backup, settings, activity and pairing-data writes are now durable (the file and its folder
  are flushed to disk), and external helper processes run with bounded time and output.
- Windows: the scanner listener port can no longer be shared with another program.

### Fixed
- History: reversing a commit can no longer write an invalid snapshot (for example into a deleted
  rack) that left the database unreadable; commits are validated before they are stored.
- Scanner stock changes that arrive during an edit, an import or a failed save are kept instead of
  being overwritten, and pending device events are applied in arrival order.
- Quantity and rack-number arithmetic saturates instead of overflowing, quantities are parsed
  strictly, duplicate Inventatory IDs are repaired instead of blocking saves, and an edit no longer
  keeps a rack slot another part took in the meantime.
- Restore and backup keep inventory history, reject unsafe or unreadable folders without throwing,
  keep a linked data folder path, and roll back identically on every failure.
- Importing: CSV files with a byte order mark, UTF-16 text, or quotes inside quoted cells are read
  correctly, a reviewed import asks before it is discarded, and DigiKey rows are classified by whole
  words with the real Package / Case parameter and refreshed tokens.
- Exports are written atomically (a full disk no longer leaves a truncated file).
- Part values: the ohm sign and overflowing numbers are handled in value parsing.
- Scan setup wizard: each step starts empty (the Wi-Fi password no longer carries into the next
  step), secrets are cleared on every page exit, and R retries saving pairing data.
- Leaving a screen asks about unsaved changes first; the exit prompt can no longer offer
  "Exit anyway" after it was cancelled; opening DigiKey setup keeps staged Settings edits; a pending
  rack move is dropped when opening part details; input is ignored while the resize notice is shown.
- A Ctrl+Z on Projects undoes a finished build, Enter keeps the page action after a mouse click,
  the action sheet scrolls and has a clickable Actions control, and typing replaces a prefilled
  quantity.
- Wide characters are measured by display width in tables and wrapped text.
- Printers: failures show one clear line instead of raw output, and unknown Windows printer states are
  reported as not ready.
- Linux: the background service starts without a UTF-8 locale, abandoned update download folders are
  removed, the installer updates an executable whose path ends in a deleted suffix, subprocesses no
  longer inherit file descriptors, and the DigiKey transport no longer uses signals.
- Windows: paths and error messages are UTF-8 end to end (environment, file and folder dialogs,
  case-insensitive backup path comparison), your proxy settings are honoured, transient sharing
  violations on file replacement are retried, a relative inventory folder is rejected, the uninstaller
  removes stored credentials and leftovers, the installer ignores other users' processes, and a
  background service is only force-stopped from the same session.
- The software update check also checks Inventascan firmware again.

### Changed
- Faster saves and startup: commit history is validated while streaming and not repeated after a
  save, an unchanged database is not rewritten at startup, and merged items are looked up by id.
- The Windows desktop shortcut is left to the installer.
- About 2,600 lines of dead or duplicated source were removed (legacy scan and quantity queues, unused
  console API and helpers, redundant includes); behavior is unchanged.
- Tests: the core test program is split into named tests (`--list`, `--filter`), uses private
  temporary folders and free ports, and has stronger assertions. CI now also builds with Clang and
  with AddressSanitizer/UndefinedBehaviorSanitizer and smoke-tests the release binary.

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
