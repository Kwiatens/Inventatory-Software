# Inventatory User Guide

## Navigation

Use `1` Stock, `2` Racks, `3` Import, `4` Projects, `5` History, or `6` Settings from any normal
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
commit, leaving existing history untouched. `Ctrl+Z` undoes the newest change that
has not been undone yet, even after a restart. Each press steps back one change, so
pressing it again undoes the change before, and it never re-applies a change it
undid. Save pending inventory changes before using History actions.

## Stock workflow

Open Stock and type `/` to filter by part, category, tag, parameter, location,
SKU, status, or quantity expression. Select a row to see its complete details.
Use New, Edit, `-`, `+`, or Print for frequent work; links and administrative
commands are available through Actions.

Editing happens in the detail side of Stock. Choose a field, type its value, and
save the working copy with `s`. Escape cancels the active field or the edit.

A part added by hand can take its details from DigiKey, TME or Mouser. Enter the part number in **SKU**, then choose
**Vendor** in the form (Enter, or click it). Inventatory asks each vendor that is set up, and shows the answers
side by side with what each one would fill in. Choose the vendor the part came from; the part keeps that vendor
for its refreshes. A vendor is chosen once per part.

A Mouser part shows its Mouser part number as **Mouser** in its details. Press `s` on a part with a Mouser part number to
open its Mouser product page. Refresh for Mouser parts is in the Mouser section of Settings (see Settings workflow).

Use **Start stocktake** from Actions (`Space`, then the action, or `t`) for a
guided physical count. Inventatory clears the stock filter, keeps each count in
memory, and requires every part to be counted before Finish is enabled. Enter a
count for the selected part, use `j`/`k` or Up/Down to move, then press `s` to
save the session. `q` or Escape cancels without changing stock. Corrections are
recorded in the stock movement history and the selected part shows its recent
movements below the normal details.

A scan adds or finds a part only from the main code of a bag: the large Data Matrix on a DigiKey bag, or the large
QR code on a TME bag. Scan one code per bag. Inventatory reads the vendor from that code and records it as the
part's origin. A code from no supported vendor is refused with a message, and that part is added by hand. Scanning a
Mouser bag is not supported yet: its code is refused in the same way, and the part is added by hand.

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
DigiKey order CSV goes to the review list, and a KiCad BOM, an EasyEDA BOM export or
an Altium Designer BOM report goes to Projects. A BOM needs a Designator column, a
Footprint column, and a value column: Designation or Value for KiCad, Name, Value or
Comment for EasyEDA, Comment or Description for Altium. A KiCad BOM is refused, with
the row to fix named, when a part row has designators but no value. A mounting hole
or test point without a value is skipped with a warning.

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

Altium Designer: export the Bill of Materials report as CSV, choosing the columns
Comment (or Description, in an Altium 365 export), Designator, Footprint and Quantity,
and add Supplier 1, Supplier Part Number 1 and Manufacturer Part Number 1 when the parts
have them. Inventatory finds each column by its header name, so the order of the columns
does not matter. A row with no Comment or Description uses its Name. The Designator,
Description, Quantity, supplier and manufacturer names match Altium 365 exports. Comment, Footprint and LibRef have not been checked against an
Altium desktop export yet. When the Designator or the value column cannot be found, the
message names it. A report is refused with the same rules as an EasyEDA export, and a
total row at its foot is skipped. A report that has a Qty column is read as a KiCad BOM,
so its supplier columns are not read.

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
the line's value and package. Shortages are looked up on DigiKey in the background. TME is asked when DigiKey has no part for the line or is not
set up. Mouser is asked only when DigiKey and TME have no part for the line, or are not set up, and only by part
number, so a line without one is not looked up there. A part TME or Mouser cannot order is never suggested, and a part
Mouser lists under several products is not suggested either. A
suggestion that does not fit the line is marked check before ordering. `o` writes
the shopping list beside the BOM, with Suggested part, Supplier and Suggestion check columns.

Mouser allows 30 requests a minute and 1,000 a day. Inventatory spaces all its Mouser requests together so that they stay under the
per-minute limit. A large shortage run can use up the daily allowance. When Mouser reports that its limit is reached,
the run stops asking Mouser, and the lines that only Mouser could answer are looked up again on a later run.

Known limit: a line that was answered "no match" before Mouser was set up keeps that answer, and Inventatory does not
ask Mouser about it.

`+` and `-` change the board count. `f` starts the build walkthrough. It stops at
one rack at a time, pulsing the slots holding parts this build needs, and lists
what to take out of each. Only confirmed lines are picked. Enter advances, and
Backspace goes back. Parts that live outside a rack come last, grouped by location.
At the end, answer whether to subtract the picked parts from stock. Stock is checked
again before anything is subtracted. If a part changed, or has fewer units than the
walkthrough takes, nothing is subtracted and the message names the part. Ctrl+Z
undoes a deduction when it is the newest change. After other changes, press Ctrl+Z
once for each of them, then once more for the deduction (see Inventory history).

Projects persist in the inventory database, so reopening the app restores the
analysis against current stock with no re-upload. `d` forgets one.

## Settings workflow

Settings has nine categories. Changes to data location, printer, Quick Labels, the Inventatory Scan
R1 service port, auto-label behavior, and DigiKey, TME and Mouser configuration are staged until Save. Cancel
restores the saved values. Printer, DigiKey, TME and Mouser tests use the staged values.

The Quick Labels category owns up to twelve shared cable-flag texts. Add, edit, remove, reorder, test-print, then Save;
the paired R1 receives the saved list during its next sync. Selecting a preset on the R1 prints immediately through the
configured PC printer. Each cable flag prints normally oriented text on both folded halves, with its font scaled to fit.
Failed/offline device requests are not queued.

Changing the data directory saves the current inventory first and switches only
after the new location is validated. An R1 service-port change takes effect on the
next launch. DigiKey, TME and Mouser secrets are stored in the operating system credential store (Windows Credential
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
country, language and currency, stored in the settings file. **Test** requests an access token with the token and secret, then makes one product request. **Refresh inventory data** looks up each TME-sourced stock item by its TME symbol when the item has one, and otherwise
by its manufacturer part number (or its SKU when it has none), and applies a result only when the manufacturer part
number matches exactly and one product fits. The refresh also reads each part's parameters, datasheet, stock and
lowest price from TME, so a TME part shows the same details a DigiKey part does.
Refresh looks up only the parts that came from TME. A part that came from DigiKey is never looked up at TME. A TME
part number typed on a DigiKey part is kept beside it and still identifies that part when its TME label is scanned.
If the TME record gives a resistance, capacitance or inductance that differs from the stored value, the record is not
merged; the refresh reports the part as an error instead.
Lookups are requests to TME, so a large inventory takes a few minutes.

The Mouser section takes the Mouser API key. The key is hidden on screen and is saved to the credential store with
Save. A key that contains spaces or control characters is refused when it is entered, and it can be at most 256 bytes. The environment variable
MOUSER_API_KEY can supply the key instead: when it is set and not blank, Inventatory uses it in place of the stored key.
**Test** sends one search to Mouser. The key passes when Mouser answers that search without an error, and a reached request limit also shows the key was accepted. A rejected
key, a failed answer or no connection to Mouser fails the test.

**Refresh** looks up each part that came from Mouser by its Mouser part number, or else its manufacturer part number, or
else its SKU. It needs a Mouser key, saved in Settings or set in MOUSER_API_KEY. It makes one Mouser request per part, spaced about two seconds apart, so a large
inventory takes several minutes. A Mouser record with a manufacturer part number different from the part's is not
applied, and the part is counted as an error. When Mouser lists several products for a part number and the manufacturer
does not tell them apart, the part is counted as ambiguous and is left unchanged. The **Last refresh** row shows how
many parts were updated, not found, ambiguous and in error. A refresh stops early when Mouser reports its request limit
or rejects the key; the parts it has not reached are left unchanged.

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
