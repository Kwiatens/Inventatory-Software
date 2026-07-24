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
| Murata | MLCC, polymer capacitors | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, tolerance, ESR, dielectric |
| TDK | MLCC | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, dielectric, DF, IR, AEC-Q200 |
| KEMET/Yageo | capacitors/MLCC | CSV, XLSX | exact MPN, series/base alias | capacitance, voltage, tolerance, dielectric, MSL, AEC |
| Vishay | current-sense resistors | CSV, XLSX | exact MPN only when exported; series remains series | resistance, power, tolerance, TCR |
| Nexperia | diodes, BJTs, MOSFETs | CSV, XLSX | type/base plus explicit orderable variant | voltage/current ratings, VF, RDS(on), qualification |
| Texas Instruments | amplifiers, MOSFETs, timers | CSV, XLSX | generic/base plus orderable MPN | supplies, offset, bandwidth, slew, VDS, RDS(on) |
| Analog Devices | precision amplifiers | CSV, XLSX | model/base plus orderable variant | supplies, offset, bias, bandwidth |
| Microchip | MCUs, amplifiers | CSV, XLSX | device/base plus orderable variant | memories, pins, supply, clock, ADC |

Current compiled import support parses CSV. XLSX is recognized and produces an actionable error in builds without a workbook reader; it never invokes Excel or converts identifier cells. This is a known implementation limitation rather than silent CSV conversion.
