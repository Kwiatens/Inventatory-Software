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
open its Mouser product page. Refresh for Mouser parts is **Refresh all** in Settings -> Vendors (see Settings workflow).

A part with a DigiKey, TME or Mouser number shows a **Prices** section in its details. Press `c` to check prices: each
vendor that is set up and knows the part is asked, and its answer is saved. Each line shows the vendor, the packaging
(cut tape, reel, Digi-Reel), the price for the smallest quantity, the stock, and where buying starts when a minimum
order applies. Click a line to open that product on the vendor's site, or press `g`, `l` or `s` for the DigiKey, TME or
Mouser page. A line says **Old price** when it
was checked more than 24 hours ago, **Other currency** when the vendor answered in a currency other than the one set in
Settings, **End of life** when the manufacturer has stopped making it but the vendor still sells it, and **Not sold** when the
vendor no longer sells it. A check that fails keeps the prices saved before.

All prices are without VAT. TME sends prices with VAT and the rate; Inventatory takes the VAT out so they compare with
DigiKey and Mouser. DigiKey answers in the chosen currency only when its site sells in it: with the site set to US it
answers in USD whatever currency is chosen, and its lines then say **Other currency**. Mouser always answers in the
currency of your Mouser account; change it there to match. Saved prices are kept in `vendor-offers.db` in the data
folder. They are not part of backups, because a check fills them again.

Use **Start stocktake** from Actions (`Space`, then the action, or `t`) for a
guided physical count. Inventatory clears the stock filter, keeps each count in
memory, and requires every part to be counted before Finish is enabled. Enter a
count for the selected part, use `j`/`k` or Up/Down to move, then press `s` to
save the session. `q` or Escape cancels without changing stock. Corrections are
recorded in the stock movement history and the selected part shows its recent
movements below the normal details.

A scan adds or finds a part only from the main code of a bag: the large Data Matrix on a DigiKey or Mouser bag, or the
large QR code on a TME bag. Scan one code per bag. Inventatory reads the vendor from that code and records it as the
part's origin. A code from no supported vendor is refused with a message, and that part is added by hand.

A Mouser bag names no Mouser part number in its Data Matrix. Inventatory takes the customer part number printed there,
which Mouser fills with the manufacturer part number, and looks it up on Mouser together with the manufacturer on the
bag. A part is filled in only when exactly one Mouser product fits. When you entered your own number in the Mouser
basket, or Mouser has no such product, the part stays as "Scanned Mouser Item" until you add its details by hand. The
linear codes on a Mouser bag are not scanned.

A part that a scan creates prints its label after DigiKey, TME or Mouser has supplied its details, so the label carries the
part's name, value and package. When the supplier finds no part, no label prints and the status line says why;
print it from the part's details once the details are added.

Each part can have its own reorder threshold, set with **Reorder at** in Edit. A positive value
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
does not matter. A row with no Comment or Description uses its Name. The column names
match Altium Designer reports and Altium 365 exports. Part numbers are read from
Manufacturer Part Number 1, MPN, and the LCSC Part # column (an LCSC number is
taken as an LCSC part). When the Designator or the value column cannot be found, the
message names it. A report is refused with the same rules as an EasyEDA export, a
total row at its foot is skipped, and so is a heading row with no designator, with a
warning. An Excel workbook is not read; export the report as CSV. A report that has a Qty column is read as a KiCad BOM,
so its supplier columns are not read.

## Project workflow

A BOM becomes a project on the Projects page. Opening a project shows its overview:

- **Find in racks** (`f`) is the main action. It leads you to every slot that holds a part for this board.
- **In stock** counts the parts you have for the chosen board count, and says where they are: in racks, in other
  places (a location with no rack slot), or with no location at all.
- **To sort out** lists four steps that get the parts you do not have yet. Each row says what is left, and Enter opens
  it. You can do the steps in any order, or not at all.

`t` shows all lines in one table, grouped as In stock, To confirm and Not in stock. Enter on a line opens the step it
belongs to. `+` and `-` change the board count. `o` (in the Actions menu) still writes the plain shopping list beside
the BOM.

A line counts as in stock without asking only when a stock part carries the line's manufacturer part number, supplier
part number or DigiKey number, or when exactly one stock part fits the line's value and package. Nothing is looked up
on a store when a project opens. Searches run only in the Choose parts to buy step, one part at a time.

### Confirm matches

For each line where stock may have the part, the step shows the stock parts that fit, with their place and stock.
Enter uses the highlighted part, `n` says none of them is right (the line then goes to the parts to buy), and `s`
skips the line. A confirmation holds for as long as that stock part does not change.

### Choose parts to buy

The step goes through the parts that are not in stock, one at a time. It searches the project's store and lists only
parts that are in stock there at a normal price: up to 20 in the store's currency, or at most ten times what the
pieces you need cost. Parts that fit the line come first, and the top one is marked `Best`, so Enter takes it. A part the manufacturer has stopped making is marked `End of life`: the store still sells it while stock
lasts, so it can be taken, but it is never marked `Best`, and the highlighted part says to find another part if you
can. Parts
the checks cannot confirm are listed under May not fit. Parts of another value (a 100nF capacitor for a 10uF line) are
left out whenever parts that fit are found. A line that many parts fit (a pin header, a tactile switch, an LED without
a colour) asks you to choose one. Below the list, the highlighted part is shown in full: its maker, the store's
description, its details, the store's stock and what your pieces cost. A search that fails says why; `/` and Enter try
it again.

- `l` opens the highlighted part on the store's site, so you can check it first.
- `/` searches for other words. A resistor value is read as written, so `R280` is 0.28 ohm and `280R` is 280 ohm.
- `v` changes the store. A new project starts with the store you used last. Mouser is not searchable yet.
- `s` skips the part.

A part you chose is kept, and is used for the same line in your other projects. Stepping over a chosen part spends no
store requests: Enter keeps it, and `/` searches again.

### Order

The step lists the chosen parts with their order quantity and cost, and the total. `c` saves a cart file for each
store: TME's cart reads it with "Add products from a file", and DigiKey's cart upload asks which column holds the part
number and the quantity. `l` opens the next part's page, and Enter opens the highlighted one. A part that can no longer
be bought at a normal price shows Not buyable now and is left out of the cart file. `m` marks the parts as ordered,
after one question.

### Receive

The step lists the ordered parts. For each one it shows the stock part it adds to, or the new stock part it creates,
and the rack slot it goes to. Enter asks how many pieces arrived, and `a` receives everything as ordered. The part is
then in stock for this line, so the next Find in racks picks it up. Ctrl+Z undoes a receipt like any stock change.

Inventatory counts its requests to DigiKey, Mouser and TME. It warns you when a daily limit is 80% used, and when a
limit is reached it says which vendor and when it is available again. Each vendor page in Settings has a Usage row. See
[vendor-rate-limits.md](vendor-rate-limits.md).

### Find in racks

`f` starts Find in racks from the overview or the line table. When some parts are not sorted out yet, a notice first
lists them, one row each with its package, where it stands and its place on the board. Start anyway (Enter) goes on
without them; Get them first (`s`) opens the first step with work left. The walkthrough stops at
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

**Settings -> Integrations / Vendors** holds everything about DigiKey, TME and Mouser on one page. Each vendor has an
**Enabled** switch (Enter toggles it), an **API key** row that reads Hidden, with **Show** (`v`), **Edit** (`e`) and **Test** (`t`), and a **Usage** row. Show, or Enter on the row, asks for a second Enter and then lists the stored keys until the cursor moves away. A
vendor that is off keeps its stored keys but is skipped for price checks, refreshes and scans. Below the vendors, **All
vendors** holds **Currency** (EUR, PLN or USD, also asked during first setup), **Region** (a two-letter country code such
as DE, used as the DigiKey site and the TME country) and **Part data**, where **Refresh all** (`r`) refreshes every part
at its own vendor and skips vendors that are off or not set up. Edit opens a short form for the keys and turns the
vendor on when it is saved. Secrets are never shown.

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

The DigiKey form collects the Client ID, the Client secret and an optional Account ID, and stores the secret in the
credential store. The TME form takes the application token and the application secret. **Test** for TME requests an access
token with the token and secret, then makes one product request. A refresh looks up each TME-sourced stock item by its
TME symbol when the item has one, or else by its manufacturer part number (or its SKU when it has none), and applies a
result only when the manufacturer part number matches exactly and one product fits. The refresh also reads each part's
parameters, datasheet, stock and lowest price from TME, so a TME part shows the same details a DigiKey part does.
Refresh looks up only the parts that came from TME. A part that came from DigiKey is never looked up at TME. A TME
part number typed on a DigiKey part is kept beside it and still identifies that part when its TME label is scanned.
If the TME record gives a resistance, capacitance or inductance that differs from the stored value, the record is not
merged; the refresh reports the part as an error instead.
Lookups are requests to TME, so a large inventory takes a few minutes.

The Mouser form takes the Mouser API key. The key is hidden on screen and is saved to the credential store. A key that contains spaces or control characters is refused when it is entered, and it can be at most 256 bytes. The environment variable
MOUSER_API_KEY can supply the key instead: when it is set and not blank, Inventatory uses it in place of the stored key.
**Test** sends one search to Mouser. The key passes when Mouser answers that search without an error, and a reached request limit also shows the key was accepted. A rejected
key, a failed answer or no connection to Mouser fails the test.

**Refresh** looks up each part that came from Mouser by its Mouser part number, or else its manufacturer part number, or
else its SKU. It needs a Mouser key, saved in Settings or set in MOUSER_API_KEY. It makes one Mouser request per part, spaced about two seconds apart, so a large
inventory takes several minutes. A Mouser record with a manufacturer part number different from the part's is not
applied, and the part is counted as an error. When Mouser lists several products for a part number and the manufacturer
does not tell them apart, the part is counted as ambiguous and is left unchanged. The **Part data** row shows how
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
