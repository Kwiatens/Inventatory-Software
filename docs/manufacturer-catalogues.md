# Manufacturer Parametric Catalogues

Inventatory stores manufacturer reference data in `Documents/Inventatory/catalogues.db`, separately from the user-owned `inventory.db`. Catalogue snapshots are disposable: removing a source cascades through its parts, aliases, properties, and warnings without deleting inventory. Inventory retains only a small match overlay; explicit user fields remain authoritative.

## Local-only source policy

Murata, TDK, KEMET/Yageo, Vishay, Nexperia, Texas Instruments, Analog Devices, and Microchip are all **BrowserGuided** sources. Automatic fetch and redistribution are disabled. Inventatory opens only the published product-selector page. A download session watches the selected Downloads directory only after the user starts it, ignores partial browser files, requires a stable completed file, and stops on success or cancellation. Manual file selection remains the fallback.

Catalogue records are not part of inventory serialization, device synchronization, HTTP responses, analytics, updates, or release assets. Profile definitions and synthetic tests may be distributed; manufacturer rows may not.

## Data model

Each import creates a hash-identified snapshot with row, part, alias, property, warning, profile-version, filename, and timestamp metadata. Exact manufacturer/MPN indexes are used before explicit profile aliases. Punctuation is never stripped globally. Parts retain base device, exact orderable MPN, package, packaging variant, family/series, category, status, and source identity.

Properties retain numeric nominal/minimum/typical/maximum/tolerance fields, canonical unit, condition, qualifier, raw column/value/unit, mapping status, profile/version, and snapshot provenance. Unmapped scalar columns can therefore be preserved and remapped by future profile versions. Graphs, curves, models, and waveform datasets are intentionally out of scope.

## Profiles

| Profile | Categories | Formats advertised | Identity | Initial mapped focus |
|---|---|---|---|---|
| Murata | MLCC, polymer capacitors | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, tolerance, ESR, dielectric, dimensions, temperature |
| TDK | MLCC | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, dielectric, DF, IR, AEC-Q200, dimensions, temperature, packaging |
| KEMET/Yageo | capacitors/MLCC | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, tolerance, dielectric, DF, IR, temperature, MSL, AEC |
| Vishay | current-sense resistors | CSV, XLSX | an explicit orderable-MPN column enables exact matching; generic series tables remain series-only | resistance, power, tolerance, TCR |
| Nexperia | diodes, BJTs, MOSFETs | CSV, XLSX | type/base plus explicit orderable variant | voltage/current/surge/leakage, VF, recovery, capacitance, RDS(on), hFE, fT, qualification |
| Texas Instruments | amplifiers, MOSFETs, timers | CSV, XLSX | generic/base plus orderable MPN | supplies, offset/drift, bias/Iq, noise, GBW, slew, CMRR/PSRR, VDS/VGS/ID, RDS(on), threshold, gate charge |
| Analog Devices | precision amplifiers | CSV, XLSX | model/base plus orderable variant | supplies, offset/drift, bias/current, noise, bandwidth, slew, CMRR/PSRR, temperature |
| Microchip | MCUs, amplifiers | CSV, XLSX | device/base plus orderable variant | flash/RAM/EEPROM, pins, supply, clock, ADC/DAC, comparators, serial/CAN/USB interfaces, timers/PWM |

## Installing a catalogue

Open **Electrical Data Sources** (`6 Sources`), select a manufacturer, then choose one of these local workflows:

1. **Get catalogue** opens that manufacturer's published selector and starts a temporary Downloads watcher. The user performs the manufacturer's own export. Completed `.csv` or `.xlsx` files are detected only after their size is stable, then shown for review.
2. **Choose file** imports an existing export for the selected known profile.
3. **Map unknown file** (`M`) previews an otherwise unrecognised local table. It maps part number, optional base/package/description fields, and common electrical fields. The mapping is saved only in `catalogues.db` for that selected source and can be reused for later local exports.

Every path stops at a preview before writing a snapshot. The preview shows the source, file, format, profile, worksheet or delimiter, row count, mapped and preserved columns, warnings, and up to ten sample rows. Import results report parts, aliases, all properties, preserved unmapped properties, warnings, and rejected rows. An installed source shows its local filename, import time, profile version, property count, and warning count; it can be re-enriched or safely removed without deleting inventory.

CSV and XLSX use the same table-to-profile-to-normalization pipeline. XLSX is read locally with `miniz` (MIT licence); Inventatory never starts Excel, LibreOffice, COM automation, or a spreadsheet conversion process. The reader selects a profile-named worksheet when available, detects a useful header row, handles shared/inline text, cached formula results, blank cells and Unicode, and retains zero-padded numeric identifiers when the worksheet provides an all-zero number format (for example `0402`). Unsupported, encrypted, or malformed workbooks fail before any catalogue snapshot is created.

Every non-structural scalar column is retained even when it has no current canonical mapping. Those rows have `mapping_status = unmapped` together with their source column, raw value, profile/version and snapshot provenance. `reprocessSource()` applies a newer profile mapping directly to those retained local rows, without downloading the manufacturer export again. Navigation/image/export-metadata columns are deliberately ignored.
