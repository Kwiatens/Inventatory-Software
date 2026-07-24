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

Every path stops at a preview before writing a snapshot. The preview shows the source, file, format, profile, worksheet or delimiter, row count, mapped and preserved columns, warnings, and up to ten sample rows. Import results report parts, aliases, all properties, preserved unmapped properties, warnings, and rejected rows. An installed source shows its local filename, import time, profile version, property count, and warning count; press `D` to inspect the retained snapshot history. It can be re-enriched or safely removed without deleting inventory.

CSV and XLSX use the same table-to-profile-to-normalization pipeline. XLSX is read locally with `miniz` (MIT licence); Inventatory never starts Excel, LibreOffice, COM automation, or a spreadsheet conversion process. The reader selects a profile-named worksheet when available, detects a useful header row, handles shared/inline text, cached formula results, blank cells and Unicode, and retains zero-padded numeric identifiers when the worksheet provides an all-zero number format (for example `0402`). Unsupported, encrypted, or malformed workbooks fail before any catalogue snapshot is created.

## Manufacturer download guide (verified 2026-07-24)

The import accepts local `.csv` and `.xlsx` files, but that does **not** mean every manufacturer currently offers a full catalogue download. The button shown by Inventatory opens the verified starting page below; it never downloads or redistributes manufacturer data itself.

| Manufacturer | Verified starting page | How to obtain a usable file | Current result |
|---|---|---|---|
| Murata | [Product Search](https://www.murata.com/en-global/search/productsearch) | Accept Murata's usage notice, choose **Capacitors**, then use the product-search results. Murata exposes category and part searches; use **Choose file** only when the selected Murata tool supplies a CSV/XLSX. | No general catalogue-export control was visible on the public product-search landing page. |
| TDK | [MLCC product page](https://product.tdk.com/en/products/capacitor/ceramic/mlcc/index.html) | Select **Search by Characteristics** or **Part Number List**. TDK provides catalogues and part-number lists, but do not download a PDF: Inventatory cannot import it. | The former link was a 404. No general CSV/XLSX product-table export was visible on the public MLCC pages. |
| KEMET / Yageo | [Ceramic capacitors](https://www.kemet.com/en/us/capacitors/ceramic.html) | Use **Browse Ceramic** or a part-number search. Import a manufacturer-provided CSV/XLSX only; datasheets and K-SIM exports are not catalogue imports. | The former capacitors landing link was a 404. No general public catalogue-export control was visible. |
| Vishay | [Current sensing resistors](https://www.vishay.com/en/resistors-fixed/current-sensing/) | Filter the parametric table if needed, then press **Export as CSV** (preferred) or **Export as MS Excel** above the table. | Works. The former `/en/resistors/` link was a 404. The displayed table is primarily a series table, so exact matching requires an export that includes an explicit orderable part-number column. |
| Nexperia | [Products](https://www.nexperia.com/products) | Open a supported product family (diodes, BJTs, or MOSFETs), enter its parametric table, apply filters, then press **Download Excel**. | Works; Nexperia documents that the button exports the filtered selection. |
| Texas Instruments | [Amplifiers](https://www.ti.com/amplifiers) | Choose the component family, then **View all products** and use its parametric filters. Save only a CSV/XLSX if the current family table exposes one. | The former global parametric-search URL was a 404. TI's current public experience is family-specific; a universal Download/Excel control was not visible in this verification. |
| Analog Devices | [Product categories](https://www.analog.com/en/parametricsearch.html) | Choose **Amplifiers**, select a family, open its selection table, then use the Excel-download icon above the table. | Works; the starting page is now a category chooser rather than a single global table. |
| Microchip | [Products](https://www.microchip.com/en-us/products) | Select a product family or **product selection tools**, open a parametric chart, then use **Download Chart**. Select **All Data** when a complete chart is wanted; Microchip downloads `.xlsx`. | Works; Microchip's official guide confirms the chart-download control. |

### What to tell users

1. Select a source and press **Get catalogue**. Complete the manufacturer's own selection/export in the browser.
2. Prefer CSV where it is offered; otherwise use the manufacturer's `.xlsx`. Do not rename a PDF, HTML page, ZIP file, simulator result, or individual datasheet as a catalogue.
3. When the browser finishes the download, return to Inventatory. It detects a newly completed file in the watched Downloads folder. If the file was saved elsewhere or was downloaded earlier, press **F** and select it manually.
4. Check the preview's **Part number** field. For exact enrichment it must be an orderable manufacturer part number, not merely a series name. If the file is not recognised, press **M** and map its real part-number column before importing.

### Availability note

Manufacturer websites change frequently and several public sites no longer provide a general bulk export. Inventatory must describe those sources truthfully: a browser-guided entry point is not a promise that a downloadable whole-catalogue spreadsheet exists. The supported manual mapping path is the correct fallback for an official local CSV/XLSX whose headers do not match a built-in profile.

Every non-structural scalar column is retained even when it has no current canonical mapping. Those rows have `mapping_status = unmapped` together with their source column, raw value, profile/version and snapshot provenance. `reprocessSource()` applies a newer profile mapping directly to those retained local rows, without downloading the manufacturer export again. Navigation/image/export-metadata columns are deliberately ignored.
