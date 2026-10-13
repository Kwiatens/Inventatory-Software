# Inventatory User Guide

## Navigation

Use `1` Home, `2` Stock, `3` Racks, `4` Import, `5` Projects, `6` History, or `7` Settings from any normal
workspace. The same destinations are clickable. Press `Space` or click
`Actions` to see every command available in the current context. `Tab` moves
focus and `Enter` activates it.

On Home, the stock warnings list is active first. Use Up/Down, `j`/`k`,
PageUp/PageDown, Home, and End to scroll it; Left/Right switches between stock
warnings and Recent Commits. The mouse wheel scrolls whichever of those two
panels is under the pointer.

## Inventory history

History is a local, append-only record of inventory state. Every completed
inventory action creates one commit containing the complete items and rack
snapshot, plus field-level differences from its parent. Legacy activity and
stock-movement records remain available separately; they are not converted.

Open History with `6`. Commits are newest first. Select one to inspect its
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
Use New, Edit, `-`, `+`, or Print for frequent work; links and administrative
commands are available through Actions.

Editing happens in the detail side of Stock. Choose a field, type its value, and
save the working copy with `s`. Escape cancels the active field or the edit.

Use **Start stocktake** from Actions (`Space`, then the action, or `t`) for a
guided physical count. Inventatory clears the stock filter, keeps each count in
memory, and requires every part to be counted before Finish is enabled. Enter a
count for the selected part, use `j`/`k` or Up/Down to move, then press `s` to
save the session. `q` or Escape cancels without changing stock. Corrections are
recorded in the stock movement history and the selected part shows its recent
movements below the normal details.

A part that a scan creates prints its label after DigiKey or TME has supplied its details, so the label carries the
part's name, value and package. When neither supplier finds the part, no label prints and the status line says why;
print it from the part's details once the details are added.

Each part can have its own **Reorder threshold** in Edit. A positive value
overrides the global low-stock warning; `0` uses the global setting. The active
threshold is used by the dashboard, search, rack indicators, and stock warnings.

## Rack workflow

Select a rack, then a slot in its 5x5 grid. Place/Move starts a move from an
occupied slot and completes it on the destination. Printing, automatic
assignment, unassignment, filtering, and rack administration are in Actions.

## Import workflow

Open Import and choose a CSV file. The format is detected from its header: a
DigiKey order CSV goes to the review list, and a KiCad BOM or an EasyEDA BOM export
goes to Projects. A BOM needs a Designator column, a Footprint column, and a value
column: Designation or Value for KiCad, Name, Value or Comment for EasyEDA.

For a DigiKey order, review each candidate, correct it if needed, then Accept or
Skip. After the final row, choose whether to enrich accepted parts with DigiKey
metadata. Inventatory reports created, merged, skipped, and failed counts before
returning to inventory.

EasyEDA: in EasyEDA Standard, choose File, Export BOM, and include Name, Designator,
Footprint, Quantity, and Supplier Part. Include Manufacturer Part when the design
has it, because it identifies the part exactly. An export is refused, with the
row to fix named, when a designator appears twice, a row has no name or
designator, or a quantity is not a whole number. EasyEDA Pro CSV exports use the
same reader, but they have not been tested against a real Pro export yet. XLSX exports
are not read.

## Project workflow

A BOM becomes a project on the Projects page. Each line is compared with stock:

- Ready: a stock part is confirmed for the line, and enough units are on hand.
- Check: stock has parts that fit the line, but the choice is yours. Press `a` to
  step to the next candidate, and `c` to confirm the part shown. A confirmation
  holds for as long as that stock part does not change.
- Short or Missing: nothing in stock covers the line, or the confirmed part does
  not have enough units.

A line is chosen without asking only when a stock part carries the line's
manufacturer part number, supplier part number or DigiKey number, or when exactly one stock part fits
the line's value and package. Shortages are looked up on DigiKey in the background, and on TME when DigiKey has no part for the line or is not
configured. A part TME cannot order is never suggested. A
suggestion that does not fit the line is marked check before ordering. `o` writes
the shopping list beside the BOM, with Suggested part, Supplier and Suggestion check columns.

`+` and `-` change the board count. `f` starts the build walkthrough. It stops at
one rack at a time, pulsing the slots holding parts this build needs, and lists
what to take out of each. Only confirmed lines are picked. Enter advances, and
Backspace goes back. Parts that live outside a rack come last, grouped by location.
At the end, answer whether to subtract the picked parts from stock. Stock is checked
again before anything is subtracted. If a part changed, or has fewer units than the
walkthrough takes, nothing is subtracted and the message names the part. Ctrl+Z
undoes a deduction.

Projects persist in the inventory database, so reopening the app restores the
analysis against current stock with no re-upload. `d` forgets one.

## Settings workflow

Settings has five categories. Changes to data location, printer, Quick Labels, the Inventatory Scan
R1 service port, auto-label behavior, and DigiKey and TME configuration are staged until Save. Cancel
restores the saved values. Printer, DigiKey and TME tests use the staged values.

The Quick Labels category owns up to twelve shared cable-flag texts. Add, edit, remove, reorder, test-print, then Save;
the paired R1 receives the saved list during its next sync. Selecting a preset on the R1 prints immediately through the
configured PC printer. Each cable flag prints normally oriented text on both folded halves, with its font scaled to fit.
Failed/offline device requests are not queued.

Changing the data directory saves the current inventory first and switches only
after the new location is validated. An R1 service-port change takes effect on the
next launch. DigiKey and TME secrets are stored in the operating system credential store (Windows Credential
Manager, or the Secret Service on Linux).

In **Settings -> General / Data**, **Background & startup** controls whether Inventatory remains available for Scan R1 after
the terminal is closed. It defaults to Off. When enabled, Inventatory starts for
the signed-in Windows user, hides in the notification area after close, and continues the R1 service. Use the Inventatory tray
icon to Open Inventatory or Quit Inventatory; the Windows startup entry is shown as **Inventatory Background Service**. Disabling the setting removes Inventatory from Windows startup. This does not run before a
user signs in.

An unconfigured Inventatory Scan section shows only **Begin Setup**, which opens
the existing setup wizard. After provisioning is accepted, the section restores
**Pair new device**, the R1 status line (online/offline, device id, signal, and
last contact once it has reported in), the service port, and **Check for firmware
updates**. Before the first report, the status reads **Waiting for device**.

An unconfigured DigiKey section also shows only **Begin Setup**. Its wizard
collects the Client ID and Client secret, stores the secret in Windows
Credential Manager, and restores the account and regional settings after the
credentials are saved. A live credential test remains available afterward.

The TME section takes the application token and application secret, stored in the credential store, and the
country, language and currency, stored in the settings file. **Test** requests an access token with the token and secret, then makes one product request. **Refresh inventory data** looks up each stock item by its TME symbol when the item has one, and otherwise
by its manufacturer part number (or its SKU when it has none), and applies a result only when the manufacturer part
number matches exactly and one product fits. The refresh also reads each part's parameters, datasheet, stock and
lowest price from TME, so a TME part shows the same details a DigiKey part does.
An item that already uses DigiKey keeps DigiKey as its selected source, and the TME record is kept beside it.
Lookups are requests to TME, so a large inventory takes a few minutes.

Pairing maintenance stays on the Actions sheet (`Space`): re-pair after a
scanner firmware update, regenerate the pairing secret, or clear the device.
Regenerating the secret invalidates the old pairing, and clearing a device
removes its paired identity.

The R1 communicates directly with the authenticated device service inside the
desktop application. This is not the retired phone/web scanner: Inventatory exposes no
browser scanning page, and the service accepts only the paired R1 device API.

For a new or reset R1, use **Settings → Inventatory Scan → Begin Setup**, then
choose **Find scanner** inside the wizard. Enter the six-digit code shown on the
R1 and the home Wi-Fi credentials.
Bluetooth LE Secure Connections encrypts and authenticates that transfer;
Bluetooth setup then turns off and normal mDNS discovery starts. Sync traffic
uses the paired secret to authenticate every request and response, so an mDNS
advertisement alone is not trusted. Hold `#` on the R1 to erase only
provisioning and return to this flow; queued inventory events stay intact.

On an idle R1, press `C` for the device menu. **Quick Labels** automatically checks the paired PC for the latest
presets when opened (use `D` for the next page). **Quick Settings** changes LCD contrast, enters standby, starts OTA,
or starts a confirmed re-pair.
