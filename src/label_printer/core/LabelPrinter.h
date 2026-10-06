// Inventatory - Hardware Inventory Management System
// Zebra label generation and printer queue integration.

#pragma once

#include "core/inventory/Inventory.h"
#include "label_printer/symbols/RackSymbols.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::optional;
using std::string;
using std::unique_ptr;
using std::vector;

struct PrinterQueueInfo {
  string name;
  string driverName;
  string portName;
  string statusText;
  bool isDefault = false;
  bool isReady = false;
};

struct PrinterCheckResult {
  bool ok = false;
  string message;
};

struct LabelParameterTile {
  string caption;
  string value;
};

struct InventatoryLabelPlan {
  string categoryHeader;
  // Passive parts print a measured value (10kΩ, 100nF) with its tolerance
  // beside it; every other part prints its part number or name.
  string mainValue;
  string mainTolerance;
  bool mainIsMeasuredValue = false;
  string packageLine;
  string manufacturerLine;
  vector<LabelParameterTile> parameters;
  string inventatoryId;
  string scannerHint;
  string barcodeHint;
  string rackLocation;
  string rackCode;
  string rackCell;
};

struct InventatoryRackLabelPlan {
  // The rack type as stored, upper-case ("INTEGRATED CIRCUITS"), and the short form printed when the full name
  // does not fit nicely ("ICs"); empty when the type has no short form.
  string categoryText;
  string shortCategoryText;
  // The rack number without the R prefix, padded to two digits ("01", "12", "128").
  string rackNumber;
  // Key of the electrical symbol drawn for the type ("resistor", "grid" for custom types).
  string symbolKey;
};

class PrinterBackend {
 public:
  virtual ~PrinterBackend() = default;

  virtual vector<PrinterQueueInfo> enumeratePrinters() const = 0;
  virtual PrinterCheckResult probePrinter(const string& printerName) const = 0;
  virtual bool sendRawJob(const string& printerName, const string& jobName, const string& zpl, string* error) const = 0;
};

class LabelPrinterService {
 public:
  explicit LabelPrinterService(unique_ptr<PrinterBackend> backend = nullptr);
  ~LabelPrinterService();

  vector<PrinterQueueInfo> enumeratePrinters() const;
  bool loadConfig(const filesystem::path& path);
  bool saveConfig(const filesystem::path& path) const;

  void setConfiguredPrinter(string printerName);
  const string& configuredPrinter() const;
  bool hasConfiguredPrinter() const;

  optional<PrinterQueueInfo> configuredPrinterInfo() const;
  PrinterCheckResult probeConfiguredPrinter() const;

  InventatoryLabelPlan buildLabelPlan(const InventoryItem& item, string rackLocation = {}) const;
  string buildZpl(const InventoryItem& item, string rackLocation = {}) const;
  bool printItemLabel(const InventoryItem& item, string* error, string rackLocation = {}) const;
  string buildWireLabelZpl(const string& text) const;
  bool printWireLabel(const string& text, string* error) const;
  InventatoryRackLabelPlan buildRackLabelPlan(const InventatoryRack& rack) const;
  string buildRackLabelZpl(const InventatoryRack& rack, SymbolStandard standard = SymbolStandard::Eu) const;
  bool printRackLabel(const InventatoryRack& rack, string* error, SymbolStandard standard = SymbolStandard::Eu) const;

  string summaryText() const;

 private:
  // What printer.conf held when this service last loaded or wrote it, so saveConfig can leave an
  // unchanged file alone and never replace one it could not read (a transient open failure must not turn
  // the user's printer choice into an empty configuration).
  enum class ConfigFileState { Unknown, Missing, InSync, Unreadable };

  unique_ptr<PrinterBackend> backend_;
  filesystem::path configPath_;
  string configuredPrinter_;
  mutable filesystem::path persistedPath_;
  mutable ConfigFileState configFileState_ = ConfigFileState::Unknown;
  mutable string persistedPrinter_;
  mutable bool printerChosenSinceLoad_ = false;
};

string sanitizeLabelText(const string& value);

}  // namespace inventatory
