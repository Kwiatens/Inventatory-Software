# Changelog

All notable changes to the Inventatory software are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Inventatory Scan: the large Data Matrix on a Mouser bag adds or finds a part, and Mouser fills in its details. It
  needs the Scan R1 firmware that sends Mouser bag codes. Older firmware still refuses a Mouser bag.
- Mouser ferrite beads: Mouser lists only packaging for them, so the impedance and frequency are read from the
  description, and the label shows them.
- Label parameter tiles are captioned in words (Impedance, Frequency, Capacitance, Drain current) instead of
  symbols such as Z, F, C and Id.
- DigiKey, Mouser and TME requests are counted against the vendors' published limits (DigiKey 120/min and 1,000/day,
  Mouser 30/min and 1,000/day; TME publishes none). A message appears when a daily limit is 80% used. Settings shows a
  Usage row on each vendor page. Counts are kept across restarts in `vendor-usage.conf` beside `settings.conf`.
- Stock: a part with a DigiKey, TME or Mouser number shows its prices from each vendor. Press `c` to check them. Each
  line shows the packaging, the price, the stock and the minimum order, says when a price is old or in another
  currency, and opens the product on the vendor's site. Prices are compared without VAT.
- One Vendors page in Settings for DigiKey, TME and Mouser: an on/off switch, the API key with a setup form, a test
  and the request count for each vendor, then one currency, one region and one Refresh all for every part at its own
  vendor. A vendor that is off keeps its keys but is skipped.
- One currency for all vendors, chosen during first setup and in Settings > Vendors.

### Changed
- The DigiKey and TME currency settings are now one shared currency, and the DigiKey site and TME country are one shared
  region. The language and account settings are no longer shown.
- A reached vendor limit is no longer a plain failure: the message names the vendor and says when it is available
  again, no request is sent until then, and a refresh or shortage run stops at once. DigiKey and TME 429 answers get
  the same treatment as Mouser's.

### Fixed
- Settings: after restoring a backup or changing the data folder, Quick Labels no longer shows Unsaved, and saving
  that page no longer replaces the restored Quick Labels with the old ones.

## [v0.1.7] - 2026-10-22

**Highlights**
- Added support for **Mouser** as a supplier.
- Added support for **Altium Designer** EDA BOM imports.

### Added
- Settings: a Mouser category takes the Mouser API key. Test checks the key, and Refresh inventory data adds Mouser details to
  stock items that came from Mouser.
- Stock: a part added by hand can take its details from Mouser, beside DigiKey and TME. Press `s` on a part with a Mouser
  part number to open its Mouser product page.
- Projects: a shortage line that DigiKey and TME do not match is looked up on Mouser when Mouser is set up. A part Mouser
  cannot order is never suggested.
- History: a filter for Mouser commits.

### Changed
- Projects: a shortage line is looked up on DigiKey, then TME, then Mouser. Mouser is asked only when DigiKey and TME have no
  part for the line or are not set up.
- Refresh: each part is refreshed only from the vendor it came from, Mouser included. A Mouser record with a different
  manufacturer part number is not applied.
- Projects: a Mouser suggestion saved by this version shows as a DigiKey part in an earlier version.
- Projects: Find in racks counts pieces, not parts, on its stop and finish screens.
- Projects: a KiCad BOM with a part row that has no Value is refused, with a message naming the row and its designators,
  as EasyEDA and Altium BOMs already are. Such a row used to be skipped without a word, so the part was missing from the
  shopping list. A mounting hole or test point without a value is still skipped with a warning.

### Fixed
- A TME label whose manufacturer part number contains a `*`, such as `DS1023-1*40S21`, is read when the Scan R1 sends it, instead of being refused.
- A Scan R1 event whose number the desktop already holds for a different scan is refused with a message, instead of being acknowledged and dropped. After five refusals the Scan R1 shows the sync problem `SYN-01` until the stuck scan is discarded from its menu.
- History: Ctrl+Z steps back one change per press. A second press no longer re-applies the change it undid, and the Projects comparison refreshes after an undo.
- Racks: Escape cancels a move that is in progress and stays on the Racks page, instead of leaving it.
- A part whose name is too long for the Scan R1 screen shows its short label, such as `Connector`, instead of a name cut off on the screen.
- Linux: the background service restarts after a stop it did not ask for, such as `pkill -x inventatory`, and takes over when the terminal app closes unexpectedly. The Scan R1 keeps connecting without opening Inventatory.
- Stock: Del starts deleting the selected part on every system. On Linux the advertised Ctrl+Backspace never reached
  the app. Ctrl+Backspace still works where the terminal sends it.
- The delete confirmation covers the page behind it, instead of letting the page text show through.
- Settings: the TME key Test cannot be started while a Refresh is running, as on the Mouser page.
- A failed TME or Mouser connection is reported as a TME or Mouser error, instead of a DigiKey error.
- A part name or error text with a broken character no longer makes the Scan R1 refuse the whole sync. The broken
  character shows as a replacement mark and the rest of the text stays readable.

### Security
- An inventory database that contains hidden database code, such as a backup crafted on another computer, is refused
  instead of opened. Inventatory never creates such code, so your own workspaces and backups are not affected.

## [v0.1.6] - 2026-10-16

**Highlights**

- **TME is a second supplier.** Add a part from TME, refresh TME details on stock, and look up shortage lines on TME. Set up TME in Settings.
- **EasyEDA BOM import.** Import an EasyEDA export in Projects, then press `c` to confirm each stock match.
- **Labels print the same text whichever supplier sent the part.**
- **Before you upgrade:** the first time this version opens a database, it upgrades the database. Earlier versions cannot open the upgraded file.

### Added
- Stock: a part added by hand can take its details from DigiKey or TME. Enter the part number in SKU, then choose
  Vendor in the form. The vendors that answer are shown side by side, and the part keeps the vendor you choose.
- Projects: EasyEDA BOM exports can be imported. Name, Value, Comment, Designator, Footprint and Quantity are read.
  Supplier Part and Manufacturer Part are used to find stock when the export includes them.
- Projects: press `c` to confirm the stock part shown for a line. A line that waits for confirmation is shown as Check.
- Settings: a TME category takes the TME token and application secret, and the storefront country, language and currency.
  Test checks the credentials. Refresh inventory data adds TME details to stock items that have a TME symbol, or a part
  number that TME lists exactly once.
- Stock: TME parts show their parameters, package, datasheet, product page, stock and lowest price, as DigiKey parts do.
  Press `l` for the TME product page of a part.
- Stock: the item form has a TME part field.
- History: a filter for TME commits.
- Projects: a shortage line is also looked up on TME when DigiKey has no part for it or is not configured. A part TME
  cannot order is never suggested.

### Changed
- Scanner: a scan adds or finds a part only from the main code of a DigiKey bag or a TME bag. Any other code is refused
  with a message, and the part is added by hand. DigiKey and TME bags need the Scan R1 firmware that sends their
  identifiers; a scan from older firmware is refused.
- Refresh: each part is refreshed only from the vendor it came from. A part from DigiKey is not looked up at TME, and
  a part from TME is not looked up at DigiKey.
- The inventory database is upgraded to format 2 the first time this version opens it. Earlier versions cannot open the
  upgraded file.
- Linux: the release runs on any x86-64 distribution with glibc 2.34 or newer, not only Ubuntu 24.04. It is tested on Ubuntu 22.04, Debian 12, AlmaLinux 9 and Fedora 44.
- Linux: the installer checks that the program starts before it installs or updates it, and reports the reason when it does not.
- Projects: a line is matched to stock without asking only when a stock part carries the line's part number, or when
  exactly one stock part fits its value and package. When several parts fit equally, the line waits for you to choose.
  Earlier versions chose one of them silently.
- Projects: `a` moves to the next matching part without saving it. Press `c` to confirm the part shown.
- Projects: a confirmation is saved against the stock part as it was when you confirmed it. If that part's details
  change, the line waits for you to confirm again. Projects saved by earlier versions need one confirmation per line.
- Projects: a suggestion that does not fit its line is marked "check before ordering". The shopping list names the
  supplier of each suggested part, in Suggested part, Supplier and Suggestion check columns.
- CSV export: the manufacturer part number and the TME part are added as the last two columns.
- Import: the file picker offers CSV and text exports (.csv, .tsv, .txt).

### Fixed
- Scan: a DigiKey bag creates a part with its DigiKey part number. Earlier versions stored the manufacturer part number
  in its place.
- Scan: a DigiKey or TME part is looked up only at the vendor it came from, so its details are not overwritten by
  another vendor's record.
- Scan: a label for a new part prints after DigiKey or TME has supplied its details. Earlier versions printed it at once,
  so the label showed only "Unsorted" and the scanned name. When neither supplier finds the part, no label prints.
- Labels: a part still named by its supplier's description prints its part number instead. A new part's label shows
  the rack slot it was placed in. A fuse label shows its maximum voltage, maximum current and power, as well as its rating.
- Labels: a TME resistance prints its ohm sign once. An asymmetric tolerance prints as its two limits (-20/+80%).
  A diode label shows whether it is a Schottky or a standard diode before its surge rating. A transistor label shows
  its polarity or channel, its gain and its power. A microcontroller label shows its core, clock and memory.
  A label caption uses the symbol a part's name gives (VF for a forward voltage), so long names no longer fill a tile.
  An LED's operating voltage is shown as its forward voltage.
- Labels: an LED's colour is the colour of its light. A label no longer shows the lens colour in its place, and an LED shows
  its colour, forward voltage, intensity and viewing angle, the same from DigiKey and TME.
- Labels: no caption is cut short. A long caption uses a short form (INTENSITY, Z, DCR), or its tile is left off.
- Labels: a colour starts with a capital letter, so TME's "green" prints as Green.
- Labels: a value prints the same way whichever supplier sent it. Volts, amps, watts and ohms use one scale and no
  space before the unit (0.25W from one supplier and 250 mW from another both print 250mW). Capacitance stays in µF,
  so 4700µF is not written as 4.7mF.
- Labels: a long manufacturer name prints its first word (Panasonic Industry prints Panasonic), not the full name in
  smaller type.
- Labels: a radial or axial part shows its body diameter (Ø5mm) instead of its lead style, and the same package has the
  same name from every supplier: DO41 and DO-204AL print DO-41, and TSOT26 prints TSOT-23-6.
- Labels: a package or size too long for its line shows its first word or its footprint (6.6x5.1mm). It is never cut
  off with dots.
- Labels: a part without a specific label layout shows its figures by what they measure (colour, voltage, current,
  power, frequency, temperature), not in the order its supplier lists them. A tolerance is never printed as a frequency.
- Labels: a supply range marked DC (± 2.25 to 20V DC) prints without the DC, so it fits its tile. A minimum and a
  maximum print as one range (3.8 to 32V).
- Labels: a timer or other IC is no longer taken for a crystal because its type mentions an oscillator.
- Stock: when a DigiKey or TME record is merged, a parameter is overwritten only by the parameter of the same name. A
  mounting type no longer replaces a part's type, and a lens colour no longer replaces an LED's colour.
- Racks: a sensor, a microcontroller or a part that lists its interfaces is no longer placed by those words in its
  specification list. An LED is an indicator even when TME lists it as a diode type.
- TME: a record whose resistance, capacitance or inductance disagrees with the stored part is not merged, and the
  refresh reports it. A part number listed by several makers is matched to the part's own maker, and a part number is
  matched without its punctuation, as DigiKey matches it.
- Projects: a part number matches a whole word of a stock name, so LM358 no longer matches LM3585.
- Projects: footprints named like EasyEDA's (R0402, C0402_NEW) and IPC names (CAPC1608X90N) match stock of the same package.
- Projects: finishing a build checks the stock first. If a part changed or has fewer units than the build takes, nothing
  is taken out, and the message names the part. Stock is no longer reduced below zero without a message.
- Projects: the amounts taken during a build stay with their lines when the list is re-sorted.
- Projects: a restock never adds units to a part the line has not confirmed.
- Projects: a BOM without a value column reports a BOM problem, not a DigiKey one.
- Update wizard: the release notes panel is sized to its text instead of always filling the width of the screen.

## [v0.1.5] - 2026-10-11

### Changed
- Stock: Sort / Filter is a panel under the list header that stays open while you choose. Each
  choice applies at once, and the panel shows how many parts match. The header shows the sort and
  the modification-date filter in use, so the list's state is visible with the panel closed.
- Settings: an unconfigured Inventatory Scan or DigiKey panel shows only its Begin setup button at the front.
- Projects: a BOM analysis is faster on large inventories.

### Removed
- Stock: the number keys 1 to 5 no longer pick filter options. Use the arrow keys and Enter, or click.

### Fixed
- Settings: the Scan R1 firmware row no longer reports "Check failed" in release builds. Firmware
  update checks resume once the firmware release channel is public.
- Projects: when saving the stock taken for a build fails, the build no longer stays open, so Deduct
  from stock cannot be confirmed a second time and take the same parts out twice. Press R to retry the save.
- Racks: Print, Enter and Detail on a rack slot now act on the part in that slot, whatever the Stock
  list is sorted or filtered by. A part added in Stock is selected after saving.
- DigiKey: a lookup that finds no exact match no longer fills the part with a similar-looking product.
  Control characters in DigiKey text are shown as spaces, and text with an embedded NUL is refused.
- Settings: the Discard button asks for a second press like Esc does, and a DigiKey site, language or
  currency that is too long is refused when you confirm it instead of making every save fail.
- Settings: a background service that cannot start no longer skips the printer queue change, and a data
  folder that fails to load now also restores the previous DigiKey secret and startup entry.
- Stock: `rack:r1` matches only rack R1, not R10 to R19, and `param:Capacitance=1uF` no longer matches
  11 uF. A bare number in `param:` is compared in the unit of that parameter.
- Projects: BOM values written as 10mOhm, 0.1 uF with the micro sign, or 10k with the ohm sign are
  read correctly, and a BOM that fails to import keeps the open project's analysis.
- Import: a CSV cell whose closing quote is followed by spaces is read correctly, a DigiKey import on a
  large inventory is much faster, and a quantity change that would take stock below zero is reported.
- Scanner: a failed DigiKey lookup for a scanned part is reported, and a sync that omits fields no
  longer blanks the stored DigiKey details.
- Backup: a backup that cannot be fully scanned is rejected.
- Scanner: Bluetooth setup of a Scan R1 no longer freezes the terminal, a lost reply to the setup write
  keeps the token so the scanner can recover, and one network address can no longer hold every
  connection slot of the scanner service. A nearby device flooding Bluetooth discovery no longer hides
  the real scanner.
- Settings: Test DigiKey runs in the background instead of freezing the terminal.
- Scanner (Linux): the Bluetooth pairing agent only answers the Bluetooth service, so another program
  on the system bus can no longer cancel pairing or ask for the pairing code.
- Stock: a bare search word such as `r1` finds rack R1 only, not R10 to R19, and the mouse wheel over the
  open Sort / Filter panel no longer moves the stock rows behind it, and a click on its empty space
  no longer selects the stock row behind it.
- Import and labels: very long text no longer risks overflowing the stack while label sizes, units and
  package names are read, and an embedded NUL in a stored name is kept instead of cutting the text.
- Stock: the ranked list header keeps the Filter button visible at 100 columns.
- Import: a very long text field no longer crashes inductance detection, a very long label name no
  longer stalls printing, a CSV with millions of cells is refused instead of using gigabytes of memory,
  and one oversized unused DigiKey text no longer fails the whole lookup.
- Printing (Windows): a queue that cannot be queried or a print job that fails to close is reported.
- Updates (Windows): a failed final write of the downloaded update is reported as such.
- Setup (Windows): the uninstaller lists anything it could not delete.
- Printing: a label that finished printing is kept in the record when the program shuts down or the
  workspace changes while a print is being stopped, so it is not printed a second time by mistake.
- Stock: stocktake counts wider than the quantity column are no longer clipped.
- Settings: the scanner port note no longer says that a change applies on the next launch.

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

[Unreleased]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.7...HEAD
[v0.1.7]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.6...v0.1.7
[v0.1.6]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.5...v0.1.6
[v0.1.5]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.4...v0.1.5
[v0.1.4]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.3...v0.1.4
[v0.1.3]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.2...v0.1.3
[v0.1.2]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.1...v0.1.2
[v0.1.1]: https://github.com/Kwiatens/Inventatory-Software/compare/v0.1.0...v0.1.1
[v0.1.0]: https://github.com/Kwiatens/Inventatory-Software/releases/tag/v0.1.0
