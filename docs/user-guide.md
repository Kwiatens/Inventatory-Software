# Inventatory User Guide

## Navigation

Use `1` Home, `2` Stock, `3` Racks, `4` Import, `5` Projects, or `6` Settings from any normal
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

Open Import and choose a CSV file. The format is detected from its header: a
DigiKey order CSV goes to the review list, a KiCad BOM goes to Projects.

For a DigiKey order, review each candidate, correct it if needed, then Accept or
Skip. After the final row, choose whether to enrich accepted parts with DigiKey
metadata. Inventatory reports created, merged, skipped, and failed counts before
returning to inventory.

## Project workflow

A KiCad BOM becomes a project on the Projects page. Every line is matched against
stock by manufacturer part number, or by value and package for passives, and the
result splits into what is pickable now and what has to be ordered. Shortages are
looked up on DigiKey in the background; `o` writes them to a CSV beside the BOM.

`+` and `-` change the board count and re-run the analysis. `a` cycles to the next
matching part when one line has several candidates, and that choice is remembered.

`b` starts the build walkthrough. It stops at one rack at a time, pulsing the slots
holding parts this build needs, and lists what to take out of each. Enter advances,
Backspace goes back. Parts that live outside a rack come last, grouped by location.
At the end, answer whether to subtract the picked parts from stock; Ctrl+Z undoes it.

Projects persist in the inventory database, so reopening the app restores the
analysis against current stock with no re-upload. `d` forgets one.

## Settings workflow

Settings has five categories. Changes to data location, printer, Quick Labels, the Inventatory Scan
R1 service port, auto-label behavior, and DigiKey configuration are staged until Save. Cancel
restores the saved values. Printer and DigiKey tests use the staged values.

The Quick Labels category owns up to twelve shared cable-flag texts. Add, edit, remove, reorder, test-print, then Save;
the paired R1 receives the saved list during its next sync. Selecting a preset on the R1 prints immediately through the
configured PC printer. Each cable flag prints normally oriented text on both folded halves, with its font scaled to fit.
Failed/offline device requests are not queued.

Changing the data directory saves the current inventory first and switches only
after the new location is validated. An R1 service-port change takes effect on the
next launch. DigiKey secrets are stored in Windows Credential Manager.

In **Settings -> General / Data**, **Background & startup** controls whether Inventatory remains available for Scan R1 after
the terminal is closed. The first normal launch asks for permission and defaults to Off. When enabled, Inventatory starts for
the signed-in Windows user, hides in the notification area after close, and continues the R1 service. Use the Inventatory tray
icon to Open Inventatory or Quit Inventatory; disabling the setting removes Inventatory from Windows startup. This does not run before a
user signs in.

Inventatory Scan settings lead with **Pair new device**, which opens the setup
wizard, followed by one status line for the paired R1 (online/offline, device id,
signal, and last contact once it has reported in). Below that are the service
port and **Check for firmware updates**, which compares the version the R1
reported against the latest published firmware release.

Pairing maintenance stays on the Actions sheet (`Space`): re-pair after a
scanner firmware update, regenerate the pairing secret, or clear the device.
Regenerating the secret invalidates the old pairing, and clearing a device
removes its paired identity.

The R1 communicates directly with the authenticated device service inside the
desktop application. This is not the retired phone/web scanner: Inventatory exposes no
browser scanning page, and the service accepts only the paired R1 device API.

For a new or reset R1, use **Settings → Inventatory Scan → Find scanner**.
Enter the six-digit code shown on the R1 and the home Wi-Fi credentials.
Bluetooth LE Secure Connections encrypts and authenticates that transfer;
Bluetooth setup then turns off and normal mDNS discovery starts. Sync traffic
uses the paired secret to authenticate every request and response, so an mDNS
advertisement alone is not trusted. Hold `#` on the R1 to erase only
provisioning and return to this flow; queued inventory events stay intact.

On an idle R1, press `C` for the device menu. **Quick Labels** automatically checks the paired PC for the latest
presets when opened (use `D` for the next page). **Quick Settings** changes LCD contrast, enters standby, starts OTA,
or starts a confirmed re-pair.
