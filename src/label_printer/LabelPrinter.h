// HIMS - Hardware Inventory Management System
// Zebra label generation and printer queue integration.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hims {

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

struct HimsLabelPlan {
  string categoryHeader;
  string mainValue;
  string packageLine;
  string manufacturerLine;
  string parameterLine1;
  string parameterLine2;
  string parameterLine3;
  string himsId;
  string scannerHint;
  string barcodeHint;
  string rackLocation;
};

struct HimsRackLabelPlan {
  string categoryText;
  string rackText;
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

  HimsLabelPlan buildLabelPlan(const InventoryItem& item, string rackLocation = {}) const;
  string buildZpl(const InventoryItem& item, string rackLocation = {}) const;
  bool printItemLabel(const InventoryItem& item, string* error, string rackLocation = {}) const;
  HimsRackLabelPlan buildRackLabelPlan(const HimsRack& rack) const;
  string buildRackLabelZpl(const HimsRack& rack) const;
  bool printRackLabel(const HimsRack& rack, string* error) const;

  string summaryText() const;

 private:
  unique_ptr<PrinterBackend> backend_;
  filesystem::path configPath_;
  string configuredPrinter_;
};

string sanitizeLabelText(const string& value);

}  // namespace hims
