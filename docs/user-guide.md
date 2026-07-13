# HIMS User Guide

## Navigation

Use `1` Home, `2` Stock, `3` Racks, `4` Import, or `5` Settings from any normal
workspace. The same destinations are clickable. Press `Space` or click
`Actions` to see every command available in the current context. `Tab` moves
focus and `Enter` activates it.

## Stock workflow

Open Stock and type `/` to filter by part, category, tag, parameter, location,
SKU, status, or quantity expression. Select a row to see its complete details.
Use New, Edit, `-`, `+`, or Print for frequent work; links and administrative
commands are available through Actions.

Editing happens in the detail side of Stock. Choose a field, type its value, and
save the working copy with `s`. Escape cancels the active field or the edit.

## Rack workflow

Select a rack, then a slot in its 5x5 grid. Place/Move starts a move from an
occupied slot and completes it on the destination. Printing, automatic
assignment, unassignment, filtering, and rack administration are in Actions.

## Import workflow

Open Import and choose a CSV file. Review each candidate, correct it if needed,
then Accept or Skip. After the final row, choose whether to enrich accepted
parts with DigiKey metadata. HIMS reports created, merged, skipped, and failed
counts before returning to inventory.

## Settings workflow

Settings has four categories. Changes to data location, printer, the HIMS Scan
R1 service port, auto-label behavior, and DigiKey configuration are staged until Save. Cancel
restores the saved values. Printer and DigiKey tests use the staged values.

The Printer category includes quick cable-flag labels: `5V`, `GND`, `12V`, or custom text. The label repeats the text in the opposite orientation for folding around a wire.

Changing the data directory saves the current inventory first and switches only
after the new location is validated. An R1 service-port change takes effect on the
next launch. DigiKey secrets are stored in Windows Credential Manager.

HIMS Scan status, pairing state, firmware, RSSI, last result, and recent device
diagnostics are shown in HIMS Scan settings. The token is masked; use Copy token
when provisioning a device. Regenerating it invalidates the old token, and
clearing a device removes its paired identity.

The R1 communicates directly with the authenticated device service inside the
desktop application. This is not the retired phone/web scanner: HIMS exposes no
browser scanning page, and the service accepts only the paired R1 device API.

For a new or reset R1, use **Settings → HIMS Scan → Find scanner**. Enter the six-digit code shown on the R1 and the home Wi-Fi credentials. Bluetooth LE Secure Connections encrypts and authenticates that transfer; Bluetooth setup then turns off and normal mDNS syncing starts. Hold `#` on the R1 to erase only provisioning and return to this flow; queued inventory events stay intact.
