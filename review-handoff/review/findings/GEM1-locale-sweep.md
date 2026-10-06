# Locale/ctype sweep

## Global locale state

- **Calls to `setlocale`**:
  - `src/main.cpp:109-110`: On Linux (`#ifndef _WIN32`), the application invokes:
    ```cpp
    if (std::setlocale(LC_ALL, "") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
      if (std::setlocale(LC_ALL, "C.UTF-8") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
    ```
    This configures the C runtime to use the user's environment locale (falling back to `"C.UTF-8"`) to guarantee UTF-8 terminal encoding support for the FTXUI terminal rendering loop. On Windows (`_WIN32`), `std::setlocale` is never called, leaving the Windows C runtime in the default `"C"` locale.
- **Calls to `std::locale::global`**:
  - `std::locale::global(...)` is **never called anywhere** in the codebase.
- **Implications**:
  - **C++ Streams (`std::ostringstream`, `std::istringstream`, `std::stringstream`)**: In standard C++17, default-constructed streams are imbued with `std::locale()`. Because `std::locale::global` is never invoked, `std::locale()` remains the initial classic `"C"` locale for the lifetime of the process. Consequently, stream formatting and parsing (`<<` and `>>`) on floating-point values always format and parse numbers using standard dot (`.`) decimal points, without comma localization.
  - **C Library Functions & Wrappers**: On Linux, `std::setlocale(LC_ALL, "")` modifies the C library locale, specifically `LC_NUMERIC`. Standard C functions (`strtod`, `atof`, `sscanf` with `%f`, `printf`/`snprintf` with `%f`) and C++ functions delegating to C runtime parsing (`std::stod`, `std::stof`, `std::to_string(double)`) are affected by the active `LC_NUMERIC` facet (e.g. in locales like `pl_PL.UTF-8` or `de_DE.UTF-8` that use comma decimals).
  - **Prior Fix (`b528bfb`)**: Commit `b528bfb` resolved this specific issue for physical unit search parsing by introducing `inventatory::parseClassicDecimal` (`src/core/parts/DecimalParse.h`) and imbuing `std::istringstream` with `std::locale::classic()` in `src/core/parts/PhysicalValue.cpp`, ensuring dot-decimal parsing for component values (e.g., `1.5k`, `0.1uF`) regardless of the user's `LC_NUMERIC`.

---

## Bugs

### Case-insensitive Windows path comparison using ASCII tolower on UTF-8 strings

- **Location**: `src/core/transfer/InventoryTransferValidation.cpp:100-101` and `src/core/transfer/InventoryTransferValidation.cpp:257-258`
- **Code**:
  ```cpp
  #ifdef _WIN32
    transform(lhs.begin(), lhs.end(), lhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
    transform(rhs.begin(), rhs.end(), rhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  #endif
  ```
- **Why it's a bug**: `lhs` and `rhs` are UTF-8 encoded path strings obtained from `std::filesystem::path::u8string()` / `generic_u8string()`. On Windows (`_WIN32`), filesystems (such as NTFS) are case-insensitive across full Unicode. Standard C `tolower` operates byte-by-byte and only folds ASCII `A-Z`. Multi-byte UTF-8 sequences for non-ASCII characters (e.g., Polish "Łódź" vs "łódź", "Części" vs "CZĘŚCI", German umlauts "Ä" vs "ä") are not folded by single-byte `tolower`. When comparing paths that do not yet exist on disk (where `std::filesystem::equivalent` fails and falls back to string comparison), paths differing only in the casing of non-ASCII characters will compare unequal (`lhs != rhs`).
  - *Concrete input*: On Windows, comparing non-existent paths `C:\Inventatory\CZĘŚCI` and `C:\inventatory\części` causes `equivalentPath()` to return `false` instead of `true`, and `pathContains()` fails to detect directory containment / overlap during backup or restore staging validation.
- **Fix**: On Windows, compare normalized native wide paths using `_wcsicmp(firstAbsolute.lexically_normal().c_str(), secondAbsolute.lexically_normal().c_str()) == 0` (and `_wcsnicmp` for prefix containment).

---

## Safe hits

| path:line | construct | reason safe |
| :--- | :--- | :--- |
| `src/main.cpp:109` | `std::setlocale(LC_ALL, "")` | Configures terminal UTF-8 encoding on Linux; does not call `std::locale::global`. |
| `src/main.cpp:110` | `std::setlocale(LC_ALL, "C.UTF-8")` | Fallback UTF-8 locale configuration on Linux. |
| `src/app/persistence/AppBackupRestore.cpp:32` | `to_string(store_.items().size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/persistence/AppBackupRestore.cpp:52` | `to_string(suffix)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/persistence/AppBackupRestore.cpp:110` | `to_string(suffix)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/UpdateWizardPresentation.h:91` | `std::istringstream input(notes)` | Parses line strings; no floating-point formatting. |
| `src/app/UpdateWizardPresentation.h:100` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/UpdateWizardPresentation.h:129` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/UpdateWizardPresentation.h:151` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/UpdateWizardPresentation.h:168` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/query/InventoryQueryMatching.cpp:36` | `isspace(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/query/InventoryQueryMatching.cpp:209` | `isspace(static_cast<unsigned char>(character))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/persistence/AppHistoryPersistence.cpp:166` | `to_string(historyDetail_.commit.sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/app/persistence/AppHistoryPersistence.cpp:167` | `to_string(historyDetail_.commit.sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomActions.cpp:92` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomActions.cpp:94` | `to_string(item->reorderThreshold)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomActions.cpp:100` | `ostringstream out` | Formats integer counts and strings for BOM CSV export. |
| `src/app/bom/AppBomActions.cpp:159` | `ostringstream out` | Formats integer counts and strings for project build summary. |
| `src/app/bom/AppBomActions.cpp:173` | `ostringstream out` | Formats integer counts and strings for project export. |
| `src/core/transfer/InventoryTransferManifest.cpp:39` | `ostringstream output` | Formats manifest strings, sha256 hex, and integer counts. |
| `src/core/transfer/InventoryTransferManifest.cpp:252` | `isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates hex hash string. |
| `src/core/transfer/InventoryTransferManifest.cpp:257` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII hex hash string. |
| `src/core/transfer/InventoryTransferManifest.cpp:262` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates ASCII digit string. |
| `src/app/shell/AppInput.cpp:27` | `#include <regex>` | Header inclusion. |
| `src/label_printer/platform/LabelPrinterWindows.cpp:39` | `to_string(code)` | `DWORD` (`unsigned long`) argument; integer formatting is locale-invariant. |
| `src/core/bom/BomMatchHelpers.cpp:62` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates row digit string. |
| `src/core/bom/BomMatchHelpers.cpp:63` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates column digit string. |
| `src/core/bom/BomMatchHelpers.cpp:83` | `to_string(pinCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/bom/BomMatchHelpers.cpp:97` | `isalpha(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/bom/BomMatchHelpers.cpp:102` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/bom/BomMatchHelpers.cpp:111` | `tolower(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, normalizes single unit prefix letter. |
| `src/core/bom/BomMatchHelpers.cpp:175` | `isalnum(ch)` | Range-for loop variable is `unsigned char ch`; filters ASCII key characters. |
| `src/core/bom/BomMatchHelpers.cpp:176` | `tolower(ch)` | `ch` is `unsigned char`; normalizes ASCII key characters. |
| `src/core/storage/AtomicFile.cpp:42` | `ostringstream output` | Stream buffer used for reading file text. |
| `src/core/storage/AtomicFile.cpp:51` | `ostringstream output` | Stream buffer used for reading file text. |
| `src/core/storage/AtomicFile.cpp:104` | `to_string(static_cast<unsigned long long>(getpid()))` | `unsigned long long` argument; integer formatting is locale-invariant. |
| `src/core/storage/AtomicFile.cpp:105` | `to_string(static_cast<unsigned long long>(sequence))` | `unsigned long long` argument; integer formatting is locale-invariant. |
| `src/app/racks/AppRackActions.cpp:176` | `toupper(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII rack code (e.g., 'r' to 'R'). |
| `src/app/racks/AppRackActions.cpp:177` | `to_string(rackNumberFromCode(code))` | `int` argument; integer formatting is locale-invariant. |
| `src/app/racks/AppRackActions.cpp:229` | `to_string(nextNumber)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/racks/AppRackActions.cpp:294` | `to_string(secondsLeft)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/racks/AppRackActions.cpp:364` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/racks/AppRackActions.cpp:366` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/platform/digikey/DigiKeyApi.cpp:81` | `ostringstream body` | Formats JSON OAuth request string; no floating-point values. |
| `src/platform/digikey/DigiKeyApi.cpp:91` | `ostringstream out` | Formats URL-encoded string. |
| `src/platform/digikey/DigiKeyApi.cpp:145` | `ostringstream url` | Formats URL query string. |
| `src/platform/digikey/DigiKeyApi.cpp:151` | `ostringstream headers` | Formats HTTP header lines. |
| `src/platform/digikey/DigiKeyApi.cpp:170` | `ostringstream out` | Formats URL-encoded parameter string. |
| `src/platform/digikey/DigiKeyApi.cpp:189` | `ostringstream body` | Formats JSON request body. |
| `src/platform/digikey/DigiKeyApi.cpp:192` | `ostringstream headers` | Formats HTTP header lines. |
| `src/platform/digikey/DigiKeyApi.cpp:213` | `ostringstream out` | Formats URL-encoded string. |
| `src/app/AppUpdate.cpp:62` | `istringstream input(marker)` | Parses update marker lines; no floating-point values. |
| `src/app/AppUpdate.cpp:86` | `ostringstream output` | Formats progress status string with integers. |
| `src/app/AppUpdate.cpp:96` | `to_string(remaining)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/AppUpdate.cpp:97` | `to_string(minutes)`, `to_string(remaining)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/AppUpdate.cpp:117` | `to_string(process)`, `to_string(tick)` | `int` and `uint64_t` arguments; integer formatting is locale-invariant. |
| `src/app/AppUpdate.cpp:492` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/app/AppUpdate.cpp:656` | `to_string(static_cast<int>(fraction * 100.0))` | Float explicitly cast to `int` before `to_string`; integer formatting is locale-invariant. |
| `src/import/kicad/KicadBom.cpp:49` | `!isdigit(ch)` | Range-for loop variable is `unsigned char ch`; parses ASCII digits. |
| `src/import/kicad/KicadBom.cpp:101` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; parses ASCII designator suffix digits. |
| `src/import/kicad/KicadBom.cpp:127` | `isalnum(ch)` | Range-for loop variable is `unsigned char ch`; cleans ASCII footprint tokens. |
| `src/import/kicad/KicadBom.cpp:168` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/kicad/KicadBom.cpp:178` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/kicad/KicadBom.cpp:192` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/kicad/KicadBom.cpp:197` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/kicad/KicadBom.cpp:241` | `ostringstream buffer` | Stream buffer used for reading KiCad BOM file text. |
| `src/ui/pages/history/HistoryPage.cpp:255` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/core/transfer/InventoryTransferOps.cpp:108` | `to_string(timestamp)` | `int64_t` argument; integer formatting is locale-invariant. |
| `src/core/transfer/InventoryTransferOps.cpp:109` | `to_string(processId)`, `to_string(attempt)` | `int` arguments; integer formatting is locale-invariant. |
| `src/core/transfer/InventoryTransferOps.cpp:206` | `ostringstream output` | Formats operation logs with timestamps and counts. |
| `src/label_printer/platform/LabelPrinterRack.cpp:46` | `ostringstream out` | Formats integer dimensions and ZPL commands. |
| `src/label_printer/platform/LabelPrinterRack.cpp:110` | `istringstream words(name)` | Splits ASCII words by whitespace. |
| `src/label_printer/platform/LabelPrinterRack.cpp:152` | `ostringstream out` | Formats integer coordinates and ZPL commands. |
| `src/label_printer/platform/LabelPrinterRack.cpp:235` | `ostringstream out` | Formats integer coordinates and ZPL commands. |
| `src/app/persistence/AppPersistence.cpp:250` | `to_string(changedItems.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/persistence/AppPersistence.cpp:252` | `to_string(changedRacks.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/label_printer/core/LabelPrinterContext.cpp:19` | `#include <regex>` | Header inclusion. |
| `src/app/shell/AppInputModes.cpp:26` | `#include <regex>` | Header inclusion. |
| `src/app/shell/AppInputModes.cpp:182` | `isdigit(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/app/shell/AppInputModes.cpp:209` | `isdigit(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/app/shell/AppInputModes.cpp:253` | `to_string(received)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppInputModes.cpp:254` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppInputModes.cpp:271` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/platform/digikey/DigiKeyTransportLinux.cpp:101` | `std::tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII HTTP header name. |
| `src/platform/digikey/DigiKeyTransportLinux.cpp:195` | `std::ostringstream out` | Buffer for libcurl HTTP response body. |
| `src/platform/digikey/DigiKeyTransportLinux.cpp:212` | `std::ostringstream out` | Buffer for libcurl HTTP response body. |
| `src/platform/digikey/DigiKeyTransportLinux.cpp:284` | `std::ostringstream& headers` | Appends ASCII HTTP header names and values. |
| `src/platform/digikey/DigiKeyTransportLinux.cpp:293` | `std::ostringstream& headers` | Appends authorization header line. |
| `src/core/storage/InventorySqliteSchema.cpp:207` | `to_string(numeric_limits<int>::min())` | `int` argument; integer formatting is locale-invariant. |
| `src/core/storage/InventorySqliteSchema.cpp:208` | `to_string(numeric_limits<int>::max())` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/shared/AppUiShared.cpp:14` | `#include <regex>` | Header inclusion. |
| `src/ui/shared/AppUiShared.cpp:278` | `to_string(quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/shared/AppUiShared.cpp:385` | `istringstream words(text)` | Splits words by whitespace for terminal wrapping. |
| `src/ui/shared/AppUiShared.cpp:428` | `ostringstream out` | Formats wrapped lines of text. |
| `src/platform/scanner/BleProvisioningService.cpp:44` | `std::isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates hex BLE token. |
| `src/platform/scanner/BleProvisioningService.cpp:48` | `std::isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates 6-digit PIN code. |
| `src/platform/scanner/BleProvisioningService.cpp:125` | `winrt::to_string(...)` | C++/WinRT helper converting `winrt::hstring` to `std::string`, not numeric formatting. |
| `src/core/history/InventoryHistory.cpp:23` | `ostringstream out` | Serializes commit TSV record (integers and strings). |
| `src/core/history/InventoryHistory.cpp:30` | `istringstream input(line)` | Parses TSV record fields (integers and strings). |
| `src/platform/scanner/MdnsService.cpp:256` | `std::to_string(port)` | `uint16_t` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventorySelection.cpp:54` | `to_string(latest->sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventorySelection.cpp:59` | `to_string(latest->sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventorySelection.cpp:303` | `to_string(deleteConfirmationSecondsLeft())` | `int` argument; integer formatting is locale-invariant. |
| `src/core/inventory/InventorySerialization.cpp:255` | `ostringstream out` | Serializes SQLite snapshot rows (integers and strings). |
| `src/core/inventory/InventorySerialization.cpp:277` | `istringstream input(line)` | Parses SQLite snapshot rows (integers and strings). |
| `src/core/inventory/InventorySerialization.cpp:359` | `ostringstream out` | Serializes items TSV format (integers and strings). |
| `src/core/inventory/InventorySerialization.cpp:365` | `istringstream input(line)` | Parses items TSV format (integers and strings). |
| `src/core/inventory/InventorySerialization.cpp:403` | `ostringstream file` | Serializes racks TSV format (integers and strings). |
| `src/label_printer/symbols/RackSymbols.cpp:19` | `isalnum(ch)` / `tolower(ch)` | `ch` is cast to `unsigned char`; normalizes ASCII symbol keys. |
| `src/core/racks/RackAllocation.cpp:23` | `!isalnum(ch)` | Lambda parameter is `unsigned char ch`; removes punctuation from rack codes. |
| `src/core/racks/RackAllocation.cpp:101` | `toupper(static_cast<unsigned char>(...))` | Cast to `unsigned char`; checks leading 'R'/'r'. |
| `src/core/racks/RackAllocation.cpp:105` | `!isdigit(character)` | `character` is `unsigned char`; parses rack number digits. |
| `src/core/racks/RackAllocation.cpp:140` | `to_string(column)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/racks/RackAllocation.cpp:230` | `to_string(nextNumber)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/racks/RackAllocation.cpp:268` | `toupper(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII slot letter. |
| `src/core/racks/RackAllocation.cpp:300` | `to_string(column + 1)` | `int` argument; integer formatting is locale-invariant. |
| `src/platform/system/UpdateService.cpp:88` | `std::isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates hex hash string. |
| `src/platform/system/UpdateService.cpp:94` | `std::tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes hex hash string. |
| `src/platform/system/UpdateService.cpp:122` | `std::isalnum(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:123` | `std::isalnum(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:128` | `std::isalnum(ch)` | Range-for loop variable is `unsigned char ch`; validates semver tag characters. |
| `src/platform/system/UpdateService.cpp:143` | `std::istringstream input(normalized)` | Parses semver integer tokens; no floating-point values. |
| `src/platform/system/UpdateService.cpp:147` | `std::isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates semver integer digits. |
| `src/platform/system/UpdateService.cpp:167` | `std::isalnum(ch)` | Lambda parameter is `unsigned char ch`; validates asset path characters. |
| `src/platform/system/UpdateService.cpp:242` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:247` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:254` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:269` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:277` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:289` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:317` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:332` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:500` | `std::istringstream candidateInput(...)` | Parses semver prerelease integers; no floating-point values. |
| `src/platform/system/UpdateService.cpp:501` | `std::istringstream installedInput(...)` | Parses semver prerelease integers; no floating-point values. |
| `src/platform/system/UpdateService.cpp:508` | `std::isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates semver prerelease digits. |
| `src/platform/system/UpdateService.cpp:512` | `std::isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates semver prerelease digits. |
| `src/platform/system/UpdateService.cpp:551` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:556` | `std::isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/system/UpdateService.cpp:622` | `std::istringstream input(checksums)` | Parses SHA-256 checksum rows; no floating-point values. |
| `src/platform/system/UpdateService.cpp:628` | `std::istringstream row(line)` | Parses SHA-256 checksum columns; no floating-point values. |
| `src/platform/system/UpdateService.cpp:951` | `std::isalnum(ch)` | Lambda parameter is `unsigned char ch`; checks valid version character. |
| `src/platform/system/UpdateService.cpp:969` | `std::to_string(static_cast<unsigned long long>(getpid()))` | `unsigned long long` argument; integer formatting is locale-invariant. |
| `src/platform/system/UpdateService.cpp:1030` | `"printf 'Installing the Inventatory update...\\n'\n"` | Literal string inside a bash shell script template, not a C library call. |
| `src/platform/scanner/BleProvisioningServiceLinux.cpp:49` | `std::isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates hex BLE token. |
| `src/platform/scanner/BleProvisioningServiceLinux.cpp:54` | `std::isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates 6-digit PIN code. |
| `src/platform/scanner/BleProvisioningServiceLinux.cpp:59` | `std::tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII string. |
| `src/platform/scanner/BleProvisioningServiceLinux.cpp:69` | `!std::isxdigit(ch)` | Range-for variable is `unsigned char ch`; parses Bluetooth MAC hex. |
| `src/platform/scanner/BleProvisioningServiceLinux.cpp:71` | `std::tolower(ch)` | `ch` is `unsigned char`; computes hex nibble value. |
| `src/ui/pages/settings/SettingsPageInput.cpp:78` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/platform/system/StartupRegistration.cpp:31` | `std::to_string(code)` | `DWORD` (`unsigned long`) argument; integer formatting is locale-invariant. |
| `src/platform/system/StartupRegistration.cpp:47` | `std::ostringstream message` | Formats Windows error code and message string. |
| `src/ui/shared/AppUiItemDetails.cpp:9` | `#include <regex>` | Header inclusion. |
| `src/ui/shared/AppUiItemDetails.cpp:19` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes ASCII search key. |
| `src/ui/shared/AppUiItemDetails.cpp:20` | `tolower(ch)` | `ch` is `unsigned char`; normalizes ASCII search key. |
| `src/ui/shared/AppUiItemDetails.cpp:76` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes unit string. |
| `src/ui/shared/AppUiItemDetails.cpp:94` | `regex valuePattern(...)` | Pattern uses ASCII character classes; regex engine defaults to classic C++ locale. |
| `src/ui/shared/AppUiItemDetails.cpp:400` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/shared/AppUiItemDetails.cpp:416` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:78` | `to_string(count)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:83` | `tolower(character)` | Lambda parameter is `unsigned char character`; normalizes ASCII badge text. |
| `src/ui/pages/history/HistoryPageRender.cpp:115` | `to_string(commit.sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:135` | `to_string(group.indices.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:211` | `to_string(index + 1)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:394` | `to_string(commit.sequence - 1)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:397` | `to_string(commit.sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:429` | `to_string(records.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPageRender.cpp:438` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPageAppearance.cpp:40` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPageAppearance.cpp:107` | `to_string(hue)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPageAppearance.cpp:108` | `to_string(value)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:30` | `ostringstream key` | Formats UI action target ID string with integers. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:56` | `ostringstream label` | Formats record change label string. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:65` | `tolower(character)` | Lambda parameter is `unsigned char character`; normalizes ASCII action string. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:95` | `istringstream stream(value)` | Splits words by whitespace for terminal wrapping. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:113` | `isdigit(character)` | Lambda parameter is `unsigned char character`; parses digit tokens. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:130` | `toupper(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII characters. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:186` | `to_string(count)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/history/HistoryPagePrivate.cpp:202` | `to_string(commit.sequence)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppRuntime.cpp:26` | `#include <regex>` | Header inclusion. |
| `src/app/shell/AppRuntime.cpp:454` | `to_string(server_.port())` | `uint16_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPageState.cpp:143` | `to_string(settingsDraft_.lowStockThreshold)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPageState.cpp:170` | `to_string(settingsDraft_.deviceServicePort)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/shared/AppUiShared.h:25` | `using std::ostringstream;` | Type alias declaration. |
| `src/ui/pages/settings/SettingsPage.cpp:275` | `to_string(settingsDraft_.lowStockThreshold)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:395` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:408` | `to_string(printerQueues_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:417` | `to_string(index + 1)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:420` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:438` | `to_string(presets.size())`, `to_string(...)` | `size_t` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:478` | `to_string(deviceRssi_)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:480` | `to_string(static_cast<long long>(...))` | `long long` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:488` | `to_string(settingsDraft_.deviceServicePort)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:520` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:532` | `to_string(digiKeyRefreshCompleted_)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:533` | `to_string(digiKeyRefreshTotal_)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:536` | `to_string(digiKeyRefreshSucceeded_)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/settings/SettingsPage.cpp:537` | `to_string(digiKeyRefreshFailed_)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomBuildActions.cpp:82` | `to_string(max(1, rackNumberFromCode(...)))` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomBuildActions.cpp:189` | `to_string(parts)`, `to_string(pieces)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/bom/AppBomBuildActions.cpp:190` | `to_string(bomAnalysis_.boards)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomBuildActions.cpp:240` | `to_string(rows)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:31` | `ostringstream out` | Formats diff summary text using integers and strings. |
| `src/core/history/InventoryVersionDiff.cpp:43` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:44` | `to_string(item.reorderThreshold)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:54` | `to_string(item.lastUpdated)` | `int64_t` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:56` | `to_string(item.createdAt)` | `int64_t` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:79` | `to_string(rack.rows)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:80` | `to_string(rack.columns)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/history/InventoryVersionDiff.cpp:81` | `to_string(rack.createdAt)` | `int64_t` argument; integer formatting is locale-invariant. |
| `src/core/scanner/InventatoryScanProtocol.cpp:45` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates decimal integer string. |
| `src/core/scanner/InventatoryScanProtocol.cpp:56` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes device ID. |
| `src/core/scanner/InventatoryScanProtocol.cpp:272` | `ostringstream out` | Serializes authenticated JSON payload (integers and strings). |
| `src/core/scanner/InventatoryScanProtocol.cpp:342` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes hex MAC signature. |
| `src/core/scanner/InventatoryScanProtocol.cpp:349` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes hex MAC signature. |
| `src/core/scanner/InventatoryScanProtocol.cpp:430` | `ostringstream out` | Formats canonical HMAC string with integer counters. |
| `src/core/storage/InventoryStorage.cpp:192` | `to_string(enriched.changedItemCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/storage/InventoryStorage.cpp:194` | `to_string(enriched.changedRackCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:102` | `to_string(active->dimx())` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:103` | `to_string(active->dimy())` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:286` | `to_string(importSelection_ + 1)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:287` | `to_string(importCandidates_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:294` | `to_string(bomProjects_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:297` | `to_string(bomBuildStep_ + 1)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:298` | `to_string(bomBuildSteps().size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:300` | `to_string(bomAnalysis_.lines.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:301` | `to_string(bomAnalysis_.boards)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:315` | `to_string(position + 1)`, `to_string(...)` | `size_t` arguments; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:389` | `to_string(completed)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:404` | `to_string(completed)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:419` | `to_string(dispatched)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/shell/AppShellRender.cpp:431` | `to_string(bomAnalysis_.shortCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/label_printer/core/LabelPrinter.cpp:78` | `ostringstream& out` | Formats integer coordinates and ZPL commands. |
| `src/label_printer/core/LabelPrinter.cpp:83` | `ostringstream& out` | Formats integer coordinates and ZPL commands. |
| `src/label_printer/core/LabelPrinter.cpp:128` | `istringstream words(...)` | Splits sanitized label text into words; no floating-point values. |
| `src/label_printer/core/LabelPrinter.cpp:157` | `ostringstream& out` | Formats parameter tile text and integer bounds. |
| `src/label_printer/core/LabelPrinter.cpp:179` | `ostringstream& out` | Formats rack slot text and integer bounds. |
| `src/label_printer/core/LabelPrinter.cpp:222` | `ostringstream out` | Formats ZPL label commands with integers and strings. |
| `src/label_printer/core/LabelPrinter.cpp:312` | `ostringstream out` | Formats ZPL label commands with integers and strings. |
| `src/core/history/InventoryVersionHistory.cpp:22` | `istringstream input(line)` | Parses inventory change log TSV (integers and strings). |
| `src/core/history/InventoryVersionHistory.cpp:41` | `isalpha(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/history/InventoryVersionHistory.cpp:45` | `!isdigit(character)` | `character` is `unsigned char`; parses slot row digits. |
| `src/core/parts/PartDescriptorContext.cpp:19` | `isalnum(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/parts/PartDescriptorContext.cpp:20` | `tolower(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, normalizes ASCII search tokens. |
| `src/platform/scanner/HttpServer.cpp:311` | `ostringstream out` | Formats HTTP response headers and status codes. |
| `src/platform/scanner/HttpServer.cpp:323` | `ostringstream out` | Formats HTTP error response headers and status codes. |
| `src/platform/system/StartupRegistrationLinux.cpp:247` | `std::to_string(asset.size)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:51` | `to_string(importSyncCompleted_)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:192` | `to_string(candidate.item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:198` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:229` | `to_string(candidate->existingQuantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:235` | `to_string(candidate->item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:244` | `to_string(mergedQuantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:256` | `to_string(candidate->sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:266` | `to_string(importCandidates_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/import/ImportCsvPage.cpp:293` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/import/ImportCsvPage.cpp:317` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/import/ImportCsvPage.cpp:339` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/label_printer/core/LabelPrinterDetails.cpp:20` | `#include <regex>` | Header inclusion. |
| `src/label_printer/core/LabelPrinterDetails.cpp:83` | `static const regex pattern(...)` | Regex on dimensions in mm; uses ASCII digits, engine uses classic C++ locale. |
| `src/label_printer/core/LabelPrinterDetails.cpp:95` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/core/LabelPrinterDetails.cpp:96` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/core/LabelPrinterDetails.cpp:97` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/core/LabelPrinterDetails.cpp:98` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/core/LabelPrinterDetails.cpp:99` | `!isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/core/LabelPrinterDetails.cpp:142` | `regex_search(value, regex(pattern))` | Pattern uses ASCII character classes; regex engine defaults to classic C++ locale. |
| `src/label_printer/core/LabelPrinterDetails.cpp:318` | `istringstream stream(full)` | Splits package dimensions by whitespace. |
| `src/label_printer/core/LabelPrinterDetails.cpp:378` | `static const regex metricInBrackets(...)` | Regex on mm values; uses ASCII digits, engine uses classic C++ locale. |
| `src/platform/digikey/DigiKeyApiPrivate.h:102` | `std::ostringstream& headers` | Function signature declaration. |
| `src/platform/digikey/DigiKeyApiPrivate.h:103` | `std::ostringstream& headers` | Function signature declaration. |
| `src/app/common/AppActionSupport.h:45` | `isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates hex token. |
| `src/ui/pages/onboarding/OnboardingPage.cpp:403` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/app/scanner/AppScanEnrichment.cpp:100` | `to_string(max(0, request.quantity))` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppScanEnrichment.cpp:207` | `to_string(digiKeyRefreshTotal_)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppScanEnrichment.cpp:246` | `to_string(digiKeyRefreshSucceeded_)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppScanEnrichment.cpp:247` | `to_string(digiKeyRefreshFailed_)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:67` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:70` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:73` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:87` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:90` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:147` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJson.cpp:204` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/bom/AppBomProjectActions.cpp:191` | `to_string(bomAnalysis_.readyCount)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/bom/AppBomProjectActions.cpp:194` | `to_string(bomFile_.warnings.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomProjectActions.cpp:293` | `to_string(boards)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/bom/AppBomProjectActions.cpp:328` | `to_string(selectedMatch.chosen + 1)`, `to_string(...)` | `size_t` arguments; integer formatting is locale-invariant. |
| `src/app/bom/AppBomProjectActions.cpp:379` | `to_string(max(1, match.needed - match.available))` | `int` argument; integer formatting is locale-invariant. |
| `src/import/csv/CsvReader.cpp:69` | `!isspace(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/import/csv/CsvReader.cpp:184` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; parses CSV header token. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:20` | `#include <regex>` | Header inclusion. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:32` | `toupper(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII label text. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:39` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII label text. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:293` | `isalpha(ch)` | `ch` is cast to `unsigned char`; checks part code alphanumeric structure. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:294` | `isdigit(ch)` | `ch` is cast to `unsigned char`; checks part code alphanumeric structure. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:315` | `isdigit(ch)` | Checked after UTF-8 continuation check and `ch < 0x80`; ASCII digits. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:316` | `isupper(ch)` | Checked after UTF-8 continuation check and `ch < 0x80`; ASCII uppercase. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:317` | `islower(ch)` | Checked after UTF-8 continuation check and `ch < 0x80`; ASCII lowercase. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:363` | `istringstream input(trim(value))` | Splits trimmed parameter value into words; no floating-point values. |
| `src/label_printer/layout/LabelPrinterTextLayout.cpp:429` | `isspace(uch)` | Variable is `unsigned char uch`; checks whitespace in label layout. |
| `src/app/scanner/AppDeviceActions.cpp:55` | `to_string(request.protocolVersion)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:56` | `to_string(request.queueDepth)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:100` | `to_string(retried)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:122` | `to_string(discarded)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:226` | `to_string(result.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:231` | `to_string(result.appliedDelta)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:232` | `to_string(result.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:301` | `ostringstream out` | Formats UI banner string with counts and quantities. |
| `src/app/scanner/AppDeviceActions.cpp:346` | `to_string(result.appliedDelta)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:347` | `to_string(result.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:360` | `to_string(result.appliedDelta)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:361` | `to_string(result.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/scanner/AppDeviceActions.cpp:382` | `to_string(deviceRssi_)` | `int` argument; integer formatting is locale-invariant. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:62` | `isdigit(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:315` | `isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:320` | `!isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:324` | `isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:330` | `!isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:334` | `isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:343` | `!isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:347` | `isdigit(static_cast<unsigned char>(peek()))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/digikey/DigiKeyJsonParser.cpp:392` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/app/shell/AppShell.cpp:26` | `#include <regex>` | Header inclusion. |
| `src/core/scanner/InventatoryScanProtocolConfig.cpp:76` | `!isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates integer port string. |
| `src/core/scanner/InventatoryScanProtocolConfig.cpp:108` | `ostringstream output` | Serializes device scanner config file lines. |
| `src/ui/pages/stock/StockPageDetail.cpp:72` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:110` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:123` | `ostringstream distance` | Formats UI delta badge; stream defaults to classic C++ locale (`std::locale::classic()`). |
| `src/ui/pages/stock/StockPageDetail.cpp:150` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:152` | `to_string(physicalCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:175` | `to_string(movement->delta)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:178` | `to_string(movement->quantityBefore)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageDetail.cpp:217` | `to_string(detailRowsExpanded.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/label_printer/layout/LabelPrinterText.cpp:19` | `#include <regex>` | Header inclusion. |
| `src/label_printer/layout/LabelPrinterText.cpp:47` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes inductance unit string. |
| `src/label_printer/layout/LabelPrinterText.cpp:65` | `regex valuePattern(...)` | Pattern uses ASCII character classes; regex engine defaults to classic C++ locale. |
| `src/label_printer/layout/LabelPrinterText.cpp:144` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; tokenizes ASCII search key. |
| `src/label_printer/layout/LabelPrinterText.cpp:145` | `tolower(ch)` | `ch` is `unsigned char`; tokenizes ASCII search key. |
| `src/platform/scanner/HttpServerConnection.cpp:81` | `to_string(kInventatoryScanTransportProtocolVersion)` | `int` argument; integer formatting is locale-invariant. |
| `src/platform/system/BackgroundControllerLinux.cpp:81` | `std::to_string(geteuid())` | `uid_t` (integer) argument; integer formatting is locale-invariant. |
| `src/platform/system/BackgroundControllerLinux.cpp:101` | `std::to_string(getpid())` | `pid_t` (integer) argument; integer formatting is locale-invariant. |
| `src/platform/system/BackgroundControllerLinux.cpp:152` | `std::to_string(process)` | `pid_t` (integer) argument; integer formatting is locale-invariant. |
| `src/platform/system/ConsoleLinux.cpp:360` | `tolower(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`; checks desktop name against ASCII `"kde"`, `"gnome"`. |
| `src/app/settings/AppSettings.cpp:82` | `istringstream& input` | Parses quoted string setting; no floating-point values. |
| `src/app/settings/AppSettings.cpp:88` | `istringstream& input` | Parses integer setting via `stoull(..., 10)`. |
| `src/app/settings/AppSettings.cpp:103` | `istringstream& input` | Parses boolean setting string ("true"/"false"); no floating-point values. |
| `src/app/settings/AppSettings.cpp:226` | `istringstream value(...)` | Parses settings string value; no floating-point values. |
| `src/app/settings/AppSettings.cpp:356` | `ostringstream output` | Serializes settings key-value pairs (strings, booleans, integers). |
| `src/app/settings/AppSettings.cpp:407` | `istringstream value(...)` | Parses quick labels settings string value; no floating-point values. |
| `src/app/settings/AppSettings.cpp:436` | `ostringstream output` | Serializes quick label presets (strings, integers). |
| `src/app/import/AppImportActions.cpp:48` | `ostringstream buffer` | Stream buffer used for reading imported CSV file text. |
| `src/app/import/AppImportActions.cpp:89` | `to_string(importCandidates_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/import/AppImportActions.cpp:91` | `to_string(result.warnings.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app/import/AppImportActions.cpp:427` | `to_string(importCreatedCount_)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/import/AppImportActions.cpp:428` | `to_string(importMergedCount_)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/app/import/AppImportActions.cpp:429` | `to_string(importSyncedCount_)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:46` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:74` | `to_string(stocktakeCountedItems())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:75` | `to_string(store_.items().size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:78` | `to_string(filtered.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:79` | `to_string(filtered.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:125` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:227` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPage.cpp:276` | `to_string(secondsLeft)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/bom/BomMatch.cpp:90` | `!isspace(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/bom/BomMatch.cpp:119` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; checks presence of digits in BOM item name. |
| `src/core/bom/BomMatch.cpp:142` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; separates trailing part number digits. |
| `src/core/bom/BomMatch.cpp:144` | `isalpha(ch)` | Lambda parameter is `unsigned char ch`; separates part number letters. |
| `src/app/inventory/AppInventoryEdit.cpp:241` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventoryEdit.cpp:244` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventoryEdit.cpp:284` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/app/inventory/AppInventoryEdit.cpp:287` | `to_string(item->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:25` | `ostringstream out` | Formats extracted JSON string values. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:96` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:98` | `!isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:99` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:104` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:111` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:135` | `isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:242` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/scanner/InventatoryScanProtocolJsonParser.cpp:248` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/label_printer/layout/LabelPrinterZpl.h:46` | `std::ostringstream& out` | Formats integer coordinates, dimensions, and ZPL commands. |
| `src/label_printer/layout/LabelPrinterZpl.h:52` | `std::ostringstream& out` | Formats integer box dimensions and thickness. |
| `src/label_printer/layout/LabelPrinterZpl.h:60` | `std::ostringstream& out` | Formats bitmap graphics hex string and integer coordinates. |
| `src/label_printer/layout/LabelPrinterZpl.h:69` | `std::ostringstream& out` | Formats brand header text and integer coordinates. |
| `src/core/inventory/InventoryHelpers.cpp:35` | `isspace(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/inventory/InventoryHelpers.cpp:63` | `isalnum(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/inventory/InventoryHelpers.cpp:64` | `tolower(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, normalizes ASCII search tokens. |
| `src/core/inventory/InventoryHelpers.cpp:86` | `ostringstream out` | Formats integer row/column numbers into rack code. |
| `src/core/inventory/InventoryHelpers.cpp:113` | `isspace(ch)` | Lambda parameter is `unsigned char ch`; trims leading whitespace. |
| `src/core/inventory/InventoryHelpers.cpp:116` | `isspace(ch)` | Lambda parameter is `unsigned char ch`; trims trailing whitespace. |
| `src/core/inventory/InventoryHelpers.cpp:128` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII identifier. |
| `src/core/inventory/InventoryHelpers.cpp:135` | `toupper(ch)` | Lambda parameter is `unsigned char ch`; normalizes ASCII identifier. |
| `src/core/inventory/InventoryHelpers.cpp:156` | `ostringstream out` | Formats rack number with integer prefix. |
| `src/core/inventory/InventoryHelpers.cpp:164` | `ostringstream out` | Formats slot designator with row char and column integer. |
| `src/core/inventory/InventoryHelpers.cpp:177` | `istringstream input(value)` | Parses integer slot row/column. |
| `src/platform/digikey/DigiKeyProduct.cpp:12` | `#include <regex>` | Header inclusion. |
| `src/platform/digikey/DigiKeyProduct.cpp:24` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes DigiKey parameter key. |
| `src/platform/digikey/DigiKeyProduct.cpp:25` | `tolower(ch)` | `ch` is `unsigned char`; normalizes DigiKey parameter key. |
| `src/platform/digikey/DigiKeyProduct.cpp:101` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes DigiKey packaging token. |
| `src/platform/digikey/DigiKeyProduct.cpp:102` | `tolower(ch)` | `ch` is `unsigned char`; normalizes DigiKey packaging token. |
| `src/platform/digikey/DigiKeyProduct.cpp:127` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes DigiKey parameter label. |
| `src/platform/digikey/DigiKeyProduct.cpp:128` | `tolower(ch)` | `ch` is `unsigned char`; normalizes DigiKey parameter label. |
| `src/platform/digikey/DigiKeyProduct.cpp:161` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; normalizes ASCII search key. |
| `src/platform/digikey/DigiKeyProduct.cpp:162` | `tolower(ch)` | `ch` is `unsigned char`; normalizes ASCII search key. |
| `src/platform/digikey/DigiKeyProduct.cpp:172` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; tokenizes ASCII search key. |
| `src/platform/digikey/DigiKeyProduct.cpp:173` | `tolower(ch)` | `ch` is `unsigned char`; tokenizes ASCII search key. |
| `src/core/parts/PhysicalValue.cpp:42` | `// application calls setlocale(LC_ALL, "")...` | Explanatory code comment from commit `b528bfb`. |
| `src/core/parts/PhysicalValue.cpp:45` | `std::istringstream stream(literal)` | Initialized for classic decimal parsing. |
| `src/core/parts/PhysicalValue.cpp:46` | `stream.imbue(std::locale::classic())` | Explicitly imbued with classic locale to guarantee dot-decimal parsing on Linux. |
| `src/core/parts/PhysicalValue.cpp:59` | `std::isdigit(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/parts/PhysicalValue.cpp:95` | `tolower(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, normalizes unit prefix character. |
| `src/core/parts/PhysicalValue.cpp:96` | `tolower(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, normalizes unit prefix character. |
| `src/core/parts/PhysicalValue.cpp:207` | `isdigit(character)` | Lambda parameter is `unsigned char character`; checks decimal digits. |
| `src/core/parts/PhysicalValue.cpp:215` | `isalpha(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/parts/PhysicalValue.cpp:343` | `tolower(character)` | Lambda parameter is `unsigned char character`; normalizes unit string. |
| `src/platform/scanner/HttpServerLifecycle.cpp:130` | `ostringstream out` | Formats HTTP error response headers and status codes. |
| `src/ui/ActionRegistry.cpp:35` | `isalnum(ch)` / `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes action ID string. |
| `src/ui/ActionRegistry.cpp:107` | `to_string(self->selectedItem()->quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:45` | `!isdigit(ch)` | Range-for variable is `unsigned char ch`; parses CSV row quantity digits. |
| `src/import/digikey/DigiKeyCsvImport.cpp:68` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; checks if parameter value is numeric. |
| `src/import/digikey/DigiKeyCsvImport.cpp:178` | `isalnum(ch)` | Range-for variable is `unsigned char ch`; cleans DigiKey parameter keys. |
| `src/import/digikey/DigiKeyCsvImport.cpp:179` | `tolower(ch)` | `ch` is `unsigned char`; cleans DigiKey parameter keys. |
| `src/import/digikey/DigiKeyCsvImport.cpp:245` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:257` | `to_string(sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:296` | `to_string(rowIndex + 1)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:326` | `to_string(candidate.sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:327` | `to_string(candidate.sourceRow)` | `int` argument; integer formatting is locale-invariant. |
| `src/import/digikey/DigiKeyCsvImport.cpp:355` | `ostringstream buffer` | Stream buffer used for reading DigiKey CSV text. |
| `src/label_printer/platform/LabelPrinterLinux.cpp:124` | `std::tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes CUPS queue name. |
| `src/ui/pages/racks/RackManagementPage.cpp:51` | `to_string(occupied)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:53` | `to_string(capacity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:85` | `toupper(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII characters. |
| `src/ui/pages/racks/RackManagementPage.cpp:101` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:103` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:105` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:115` | `istringstream words(value)` | Splits rack description into words; no floating-point values. |
| `src/ui/pages/racks/RackManagementPage.cpp:299` | `to_string(displayRow + 1)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:483` | `std::to_string(secondsLeft)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/racks/RackManagementPage.cpp:561` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/platform/digikey/DigiKeyProductDetails.cpp:12` | `#include <regex>` | Header inclusion. |
| `src/platform/digikey/DigiKeyProductDetails.cpp:48` | `regex re(pattern, regex_constants::icase)` | Pattern uses ASCII digits and unit suffixes; regex defaults to classic C++ locale. |
| `src/platform/digikey/DigiKeyProductDetails.cpp:67` | `tolower(ch)` | Lambda parameter is `unsigned char ch`; normalizes unit string. |
| `src/platform/digikey/DigiKeyProductDetails.cpp:89` | `regex valuePattern(...)` | Pattern uses ASCII character classes; regex engine defaults to classic C++ locale. |
| `src/ui/pages/stock/StockPageInput.cpp:137` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/stock/StockPageInput.cpp:163` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/app/settings/AppSettingsAppearance.cpp:79` | `ostringstream output` | Formats hex color code string. |
| `src/app/settings/AppSettingsAppearance.cpp:87` | `!isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/scanner/HttpServerProtocol.cpp:20` | `ostringstream out` | Formats HTTP response status lines and headers. |
| `src/platform/scanner/HttpServerProtocol.cpp:48` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/scanner/HttpServerProtocol.cpp:53` | `isspace(static_cast<unsigned char>(...))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/platform/scanner/HttpServerProtocol.cpp:61` | `isalnum(ch)` | Parameter is `unsigned char ch`; validates RFC 7230 header name characters. |
| `src/platform/scanner/HttpServerProtocol.cpp:72` | `istringstream input(headers)` | Parses HTTP header lines; no floating-point values. |
| `src/platform/scanner/HttpServerProtocol.cpp:76` | `istringstream requestLine(line)` | Parses HTTP request line tokens; no floating-point values. |
| `src/platform/scanner/HttpServerProtocol.cpp:113` | `!isdigit(ch)` | Range-for variable is `const unsigned char ch`; parses HTTP Content-Length digits. |
| `src/platform/scanner/HttpServerProtocol.cpp:126` | `!isdigit(ch)` | Range-for variable is `const unsigned char ch`; parses monotonic counter digits. |
| `src/platform/scanner/HttpServerProtocol.cpp:186` | `!isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates 64-char hex fingerprint. |
| `src/platform/scanner/HttpServerProtocol.cpp:191` | `!isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates replay counter digits. |
| `src/platform/scanner/HttpServerProtocol.cpp:211` | `!isxdigit(ch)` | Lambda parameter is `unsigned char ch`; validates 64-char hex fingerprint. |
| `src/platform/scanner/HttpServerProtocol.cpp:212` | `to_string(counter)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPage.cpp:58` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/bom/BomProjectPage.cpp:83` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/core/inventory/InventoryIdentifiers.cpp:68` | `isalpha(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/inventory/InventoryIdentifiers.cpp:69` | `toupper(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, normalizes rack row letter. |
| `src/core/inventory/InventoryIdentifiers.cpp:97` | `!isdigit(static_cast<unsigned char>(ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII bytes. |
| `src/core/inventory/InventoryIdentifiers.cpp:133` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; parses slot column digits. |
| `src/core/inventory/InventoryIdentifiers.cpp:156` | `ostringstream out` | Formats rack code string with integers. |
| `src/core/inventory/InventoryIdentifiers.cpp:168` | `ostringstream out` | Formats slot coordinate string with row letter and column integer. |
| `src/core/parts/DecimalParse.h:13` | `// consulting the process locale...` | Comment documenting decimal parsing safety under non-C locales. |
| `src/core/parts/DecimalParse.h:14` | `// and std::stod would then stop at the '.'...` | Comment documenting decimal parsing safety under non-C locales. |
| `src/core/parts/DecimalParse.h:17` | `std::istringstream stream(text)` | Helper created to parse dot-decimal numbers across all platforms. |
| `src/core/parts/DecimalParse.h:18` | `stream.imbue(std::locale::classic())` | Explicitly imbued with `std::locale::classic()`, immune to `LC_NUMERIC`. |
| `src/ui/pages/scanner/InventatoryScanSetupPage.cpp:351` | `tolower(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/scanner/InventatoryScanSetupPage.cpp:387` | `isdigit(ch)` | Lambda parameter is `unsigned char ch`; validates BLE PIN digits. |
| `src/core/scanner/InventatoryScanProtocolSecurity.cpp:158` | `to_string(counter)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/core/scanner/InventatoryScanProtocolSecurity.cpp:163` | `to_string(counter)`, `to_string(status)` | `uint64_t` and `int` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageList.cpp:40` | `to_string(quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageList.cpp:116` | `ostringstream value` | Formats search match badge (`+X.X%`); stream defaults to classic C++ locale. |
| `src/label_printer/platform/LabelPrinterPlatform.cpp:56` | `to_string(processSeed)`, `to_string(...)` | `uint64_t` arguments; integer formatting is locale-invariant. |
| `src/label_printer/platform/LabelPrinterPlatform.cpp:57` | `to_string(nextSequence...)` | `uint64_t` argument; integer formatting is locale-invariant. |
| `src/label_printer/platform/LabelPrinterPlatform.cpp:205` | `ostringstream serialized` | Formats serialized label job string. |
| `src/platform/digikey/DigiKeyTransport.cpp:37` | `ostringstream out` | Buffer for libcurl HTTP response body. |
| `src/platform/digikey/DigiKeyTransport.cpp:64` | `ostringstream out` | Buffer for libcurl HTTP response body. |
| `src/platform/digikey/DigiKeyTransport.cpp:128` | `ostringstream out` | Formats OAuth request URL and body. |
| `src/platform/digikey/DigiKeyTransport.cpp:369` | `ostringstream& headers` | Appends ASCII HTTP header names and values. |
| `src/platform/digikey/DigiKeyTransport.cpp:378` | `ostringstream& headers` | Appends authorization header line. |
| `src/ui/pages/scanner/InventatoryScanSetupPageRender.cpp:60` | `to_string(device.rssi)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:73` | `isdigit(value)` | Lambda parameter is `unsigned char value`; parses BOM rack number digits. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:151` | `istringstream words(value)` | Splits words by whitespace for terminal wrapping. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:196` | `to_string(item.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:248` | `to_string(max<long long>(...))` | `long long` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:249` | `to_string(project.boards)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:264` | `to_string(bomProjects_.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:299` | `to_string(stepIndex + 1)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:300` | `to_string(steps.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:302` | `to_string(pickedPieces)`, `to_string(...)` | `int` arguments; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:309` | `to_string(totalPieces)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:310` | `to_string(steps.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:374` | `to_string(pick.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:385` | `to_string(steps[stepIndex + 1].picks.size())` | `size_t` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:413` | `to_string(pick.quantity)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:521` | `to_string(bomAnalysis_.boards)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:531` | `to_string(bomAnalysis_.readyCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:533` | `to_string(bomAnalysis_.shortCount)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:606` | `to_string(match.needed)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:607` | `to_string(match.available)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/bom/BomProjectPageRender.cpp:683` | `to_string(index)` | `size_t` argument; integer formatting is locale-invariant. |
| `src/app.cpp:26` | `#include <regex>` | Header inclusion. |
| `src/ui/pages/stock/StockPageStocktake.cpp:66` | `isdigit(static_cast<unsigned char>(key.ch))` | Argument explicitly cast to `unsigned char`, avoiding UB on non-ASCII keys. |
| `src/ui/pages/stock/StockPageStocktake.cpp:114` | `to_string(parsed)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageStocktake.cpp:169` | `to_string(changed)` | `int` argument; integer formatting is locale-invariant. |
| `src/ui/pages/stock/StockPageStocktake.cpp:184` | `to_string(countedParts)` | `int` argument; integer formatting is locale-invariant. |
