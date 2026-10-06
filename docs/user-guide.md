# Inventatory User Guide

## Navigation

Use `1` Stock, `2` Racks, `3` Import, `4` Projects, `5` History, or `6` Settings from any normal
workspace. The same destinations are clickable. Press `Space` or click
`Actions` to see every command available in the current context. `Tab` moves
focus and `Enter` activates it.

## Inventory history

History is a local, append-only record of inventory state. Every completed
inventory action creates one commit containing the complete items and rack
snapshot, plus field-level differences from its parent. Legacy activity and
stock-movement records remain available separately; they are not converted.

Open History with `5`. Commits are newest first. Select one to inspect its
parent, source, reference, changed part/rack counts, and its changed records.
The detail pane groups changes by part or rack; press `Enter` on a record to
inspect readable before/after field values, and `Esc` to return to the record
list. Long parameter changes are split into individual parameter differences.
Press `C` to create a named snapshot checkpoint. Checkpoints do not change
inventory and cannot be reversed.

Restore snapshot replaces the current items and racks with the selected commit
exactly. Reverse changes applies only the selected commit's inverse fields and
keeps unrelated later edits; it stops if the affected part or rack changed
afterward. Both operations ask for confirmation and create a new corrective
commit, leaving existing history untouched. `Ctrl+Z` performs the same kind of
durable parent restore for the latest inventory-changing commit, even after a
restart. Save pending inventory changes before using History actions.

## Stock workflow

Open Stock and type `/` to filter by part, category, tag, parameter, location,
SKU, status, or quantity expression. Select a row to see its complete details.
Use New, Edit, `-`, `+`, Delete, or Print for frequent work; links and
administrative commands are available through Actions. Delete asks for
confirmation before removing the part from the database and records the change
in History.

Press `f`, choose **filters** from Actions, or click **Sort / Filter** to open
the Stock filter menu. It can sort by quantity or name, limit results by
modification date, and be closed by pressing `f`, Escape, or the button again.
Choose **Reset filters** to restore all modification dates and A-Z sorting.
The Sort / Filter control remains available while the stock list is scrolled.

When the list is grouped by name, category and part rows use the same connected
gray branch treatment as the project comparison view.

Editing happens in the detail side of Stock. Choose a field, type its value, and
save the working copy with `s`. Escape cancels the active field or the edit.

Use **Start stocktake** from Actions (`Space`, then the action, or `t`) for a
guided physical count. Inventatory clears the stock filter, keeps each count in
memory, and requires every part to be counted before Finish is enabled. Enter a
count for the selected part, use `j`/`k` or Up/Down to move, then press `s` to
save the session. `q` or Escape cancels without changing stock. Corrections are
recorded in the stock movement history and the selected part shows its recent
movements below the normal details.

Each part can have its own **Reorder threshold** in Edit. A positive value
overrides the global low-stock warning; `0` uses the global setting. The active
threshold is used by search, rack indicators, and stock warnings.

## Rack workflow

Select a rack, then a slot in its 5x5 grid. Place/Move starts a move from an
occupied slot and completes it on the destination. Printing, automatic
assignment, unassignment, filtering, and other rack administration are in
Actions. Empty racks show a Delete rack button (also `x`); confirm with Enter
to remove the rack from the database, which is recorded in History.

A part whose category is not known yet, such as a scanned code waiting for its
DigiKey details, stays on automatic placement and receives a slot as soon as its
category is known. A part is only marked Unassigned when it cannot be racked or when
you unassign it yourself, and automatic placement never overrides that choice.

## Import workflow

Open Import and choose a CSV file. The format is detected from its header: a
DigiKey order CSV goes to the review list, a KiCad BOM goes to Projects.
The file must be UTF-8 or UTF-16 text (UTF-16 needs its byte order mark, as Excel's
"Unicode Text" writes). An old Windows code page such as Windows-1250 cannot be
recognised reliably, so Inventatory refuses it with a message instead of importing
damaged text; re-save the file as "CSV UTF-8" and import it again.

For a DigiKey order, review each candidate, correct it if needed, then Accept or
Skip. After the final row, choose whether to enrich accepted parts with DigiKey
metadata. Inventatory reports created, merged, skipped, and failed counts before
returning to inventory.

The review can stay open while a Scan R1 keeps updating stock. When the import is
saved, only the accepted rows are applied on top of the current inventory by part:
imported quantities are added to the current quantity, and parts the scanner
created are kept. Anything that could not be merged cleanly is reported when the
import is saved and listed in Activity.

## Project workflow

A KiCad BOM becomes a project on the Projects page. Open it to compare each line
with stock in the same split list/detail rhythm as Stock. Rows are grouped first
by availability (`In Stock` and `Missing`) and then by component category, such
as `Capacitors` or `Resistors`. Each row keeps Part, Package, the rack location
or suggested match, a merged Need / Have count, and Status together. The primary action is
**Find in racks** (`f`), which walks the available parts rack by rack and
highlights the slots to pick. Shortages are looked up on DigiKey in the
background; a lookup that fails (network, rate limit, credentials) is not
remembered and is retried the next time the project opens. `o` opens a save
dialog for a CSV in the active Inventatory workspace by default; the file is
written atomically, an error is reported, and lines without a DigiKey suggestion
are left blank.

`+` and `-` change the board count and re-run the analysis. `a` cycles to the next
matching part when one line has several candidates, and that choice is remembered.

`f` starts Find in racks, or previews the available picks while shortages remain.
It focuses on one rack at a time, pulsing the slots holding parts this project
needs and listing only the current picks. Enter advances and Backspace goes back.
Parts that live outside a rack come last, grouped by location. At the end, answer
whether to subtract the picked parts from stock. A finished build is an ordinary inventory
commit, so `Ctrl+Z` on the Projects page (also listed in Actions) reverts the deduction and
re-runs the comparison. Select a missing line and press `r` to open its restock flow.

Projects persist in the inventory database, so reopening the app restores the
analysis against current stock with no re-upload. `d` forgets one.

## Settings workflow

Settings has seven destinations arranged under System, Devices, and Integrations. Devices contains
Printer, with Quick Labels nested below it, and Inventatory Scan. Changes to data location, printer,
Quick Labels, the Inventatory Scan R1 service port, auto-label behavior, the label symbol standard, and DigiKey configuration are
staged until Save. Cancel restores the saved values. Printer and DigiKey tests use the staged values.

The Quick Labels panel owns up to twelve shared cable-flag texts and the optional custom wire label. Add, edit, remove,
reorder, test-print, then Save for presets; edit and print the custom label directly from the same panel.
The paired R1 receives the saved list during its next sync. Selecting a preset on the R1 prints immediately through the
configured PC printer. Each cable flag prints normally oriented text on both folded halves, with its font scaled to fit.
Failed/offline device requests are not queued.

Part labels print on 32 × 25 mm stock at 203 dpi. The black header carries the Inventatory mark and the part
category. Below it, passives show their value with the tolerance beside it (10kΩ ±1%), centred between the
header and the package row; other parts show the manufacturer part number. The package sits in a black pill
next to the manufacturer, and up to four key parameters follow as captioned tiles. The right column holds
the QR code, the short ID for typing a lookup, the rack slot as rack and cell (R12 | E3, where the letter is
the column and the number is the row), and a bracketed field for writing a new slot by hand after moving the
tube. A part that has no rack, such as a through-hole part (racks hold surface-mount, semiconductor and
similar stock), prints no slot chip; its field is captioned SLOT and enlarged instead.

Rack labels use the same 32 × 25 mm stock. A black block carries the rack number (`12`) in the largest type
that fits, with the electrical symbol for the rack's type and the type name beside it. The brand mark sits at the
right end of the header bar on part and rack labels alike. A full type name is printed when it fits nicely on one
line; otherwise a short form is used (ICs, LEDs, Crystals), and a custom type that fits neither is split over two
lines or stepped down in size, never cut. Built-in types draw the symbol for resistors, capacitors, inductors,
diodes, LEDs, transistors, ICs, crystals, fuses and connectors; a custom rack type draws a 5 × 5 grid. In
Settings > Printer, **Schematic symbols** (or `y`) switches between EU (IEC 60617) and US (ANSI/IEEE 315). Only the
resistor and the fuse differ between the two; the default is EU. The symbols come from open-licensed artwork,
rendered once by `tools/rack_symbols/generate.py`; see `tools/rack_symbols/README.md` for sources and licences.

Tiles only print values that can be trusted. A parameter must match its caption by exact name, carry the
unit the caption implies (V, A, W, Ω, Hz, °C or mm), and not describe how the part is mounted. Temperature
and voltage ranges print as `-40 to 85°C`. A value that does not fit a tile whole is left off instead of
being shortened, so a tile is either complete or absent; other text steps down in size before it is
shortened. A manufacturer name that does not fit beside the package pill drops its corporate words
(Infineon Technologies prints as Infineon) before any character is cut. Diodes, MOSFETs, transistors, TVS
diodes, fuses, switches and ICs are recognised from their parameters, not only their category, because
DigiKey files several of them under Discrete Semiconductor Products. Parts print the manufacturer part
number, not the distributor's own number.

Changing the data directory saves the current inventory first and switches only
after the new location is validated. An R1 service-port change takes effect on the
next launch. DigiKey secrets use the operating system credential service:
Windows Credential Manager on Windows and Secret Service on Linux.

In **Settings -> General / Data**, **Background & startup** controls whether Inventatory remains available for Scan R1 after
the terminal is closed. It defaults to Off. On Windows, Inventatory starts for
the signed-in user, hides in the notification area after close, and continues
the R1 service. Use the Inventatory tray icon to Open Inventatory or Quit
Inventatory; the Windows startup entry is shown as **Inventatory Background
Service**. On Linux, the setting manages a per-user systemd service. Launching
Inventatory while that service is active stops it (forcing it after a short
wait if it does not respond) and opens the terminal UI; closing the UI or its
terminal window returns the workspace to the background scanner service. Linux does not provide a notification-area icon. On both
platforms, disabling the setting removes the startup integration.

An unconfigured **Settings -> Devices -> Inventatory Scan** section shows only **Begin Setup**, which opens
the existing setup wizard. After provisioning is accepted, the section restores
**Pair new device**, the R1 status line (online/offline, device id, signal, and
last contact once it has reported in), the service port, and **Restart bridge**.
Before the first report, the status reads **Waiting for device**.

An unconfigured DigiKey section also shows only **Begin Setup**. Its wizard
collects the Client ID and Client secret, stores the secret in the operating
system credential service, and restores the account and regional settings after
the credentials are saved. A live credential test remains available afterward.

### Updates

The section lists Inventatory software, Inventascan firmware, and hardware versions in a table with a status column (up to date / update available / check failed) and the time of the last completed check. **Auto-check for updates** toggles the daily background check; like other setting changes it is staged and applied with Save.

Open **Settings → Updates** and choose **Check for software updates**. When a newer
Inventatory release, including a published GitHub prerelease, is available, the
section shows **Update to vX.Y.Z**; press
`u` or choose the same action from **Actions** to open the update wizard. It
previews the release notes, asks whether to save unsaved Settings changes,
downloads the complete package, shows bytes/speed/percentage/ETA, and verifies
the published SHA-256 hashes before handing off to the installer.

Cancel is available through download and verification. After installer handoff
Inventatory closes and restarts automatically. A successful restart shows the
complete release notes. A failed replacement leaves the previous installation
active and shows a retryable error on the next launch; inventory data and the
normal machine-local Settings file are preserved.

Pairing maintenance stays on the Actions sheet (`Space`): re-pair after a
scanner firmware update, regenerate the pairing secret, or clear the device.
Regenerating the secret invalidates the old pairing, and clearing a device
removes its paired identity.

The R1 communicates directly with the authenticated device service inside the
desktop application. This is not the retired phone/web scanner: Inventatory exposes no
browser scanning page, and the service accepts only the paired R1 device API.

For a new or reset R1, use **Settings → Devices → Inventatory Scan → Begin Setup**, then
choose **Find scanner** inside the wizard. Enter the six-digit code shown on the
R1 and the home Wi-Fi credentials.
Bluetooth LE Secure Connections encrypts and authenticates that transfer;
Bluetooth setup then turns off and normal mDNS discovery starts. Sync traffic
uses the paired secret to authenticate every request and response, so an mDNS
advertisement alone is not trusted. Hold `#` on the R1 to erase only
provisioning and return to this flow; queued inventory events stay intact.

Scanner events are applied to the saved inventory. Editing a part keeps the R1's
stock changes made while the form was open: saving applies only the fields you
changed (a quantity you typed replaces the scanned one and is reported). If an
inventory save has failed and its changes are waiting for `R` to retry, scanner
events stay queued on the desktop and are applied once the save succeeds or the
change is discarded.
While those changes wait, reload (`r`) is refused so they are not discarded; press
`R` to retry the save first.

On an idle R1, press `C` for the device menu. **Quick Labels** automatically checks the paired PC for the latest
presets when opened (use `D` for the next page). **Quick Settings** changes LCD contrast, enters standby, starts OTA,
or starts a confirmed re-pair.
