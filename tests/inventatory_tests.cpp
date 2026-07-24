#include "core/Inventory.h"
#include "app/AppSettings.h"
#include "platform/UpdateService.h"
#include "platform/StartupRegistration.h"
#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"
#include "core/CatalogueDatabase.h"
#include "core/InventatoryScanProtocol.h"
#include "core/PartDescriptor.h"
#include "import/BomCsvImport.h"
#include "import/XlsxWorkbookReader.h"
#include "label_printer/LabelPrinter.h"
#include "ui/shared/AppUiShared.h"

#include <miniz.h>

#include <cstdlib>
#include <algorithm>
#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <atomic>
#include <chrono>
#include <unordered_map>

#undef assert
#define assert(expr)                                                                                                 \
  do {                                                                                                                \
    if (!(expr)) {                                                                                                    \
      cerr << "Assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << '\n';                     \
      std::exit(1);                                                                                                   \
    }                                                                                                                 \
  } while (false)

using namespace inventatory;
using namespace std;

namespace {

bool writeSyntheticXlsx(const filesystem::path& path) {
  mz_zip_archive archive{};
  if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0)) return false;
  const auto add = [&](const char* name, const string& content) {
    return mz_zip_writer_add_mem(&archive, name, content.data(), content.size(), MZ_BEST_COMPRESSION) != 0;
  };
  const bool written =
      add("[Content_Types].xml", R"xml(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>)xml") &&
      add("xl/workbook.xml", R"xml(<?xml version="1.0"?><workbook xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Cover" sheetId="1" r:id="rId1"/><sheet name="Products" sheetId="2" r:id="rId2"/></sheets></workbook>)xml") &&
      add("xl/_rels/workbook.xml.rels", R"xml(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Target="worksheets/cover.xml" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"/><Relationship Id="rId2" Target="worksheets/products.xml" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"/></Relationships>)xml") &&
      add("xl/styles.xml", R"xml(<?xml version="1.0"?><styleSheet><numFmts count="1"><numFmt numFmtId="164" formatCode="0000"/></numFmts><cellXfs count="2"><xf numFmtId="0"/><xf numFmtId="164"/></cellXfs></styleSheet>)xml") &&
      add("xl/worksheets/cover.xml", R"xml(<?xml version="1.0"?><worksheet><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>Catalogue export</t></is></c></row></sheetData></worksheet>)xml") &&
      add("xl/worksheets/products.xml", R"xml(<?xml version="1.0"?><worksheet><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>Orderable Part Number</t></is></c><c r="B1" t="inlineStr"><is><t>Generic Part Number</t></is></c><c r="C1" t="inlineStr"><is><t>Package</t></is></c><c r="D1" t="inlineStr"><is><t>Channels</t></is></c><c r="E1" t="inlineStr"><is><t>Supply voltage (min)</t></is></c><c r="F1" t="inlineStr"><is><t>Supply voltage (max)</t></is></c><c r="G1" t="inlineStr"><is><t>Shutdown current</t></is></c></row><row r="2"><c r="A2" t="inlineStr"><is><t>OPA333AIDBVR</t></is></c><c r="B2" t="inlineStr"><is><t>OPA333</t></is></c><c r="C2" s="1"><v>402</v></c><c r="D2"><f>1+1</f><v>2</v></c><c r="E2" t="inlineStr"><is><t>1.8 V</t></is></c><c r="F2" t="inlineStr"><is><t>5.5 V</t></is></c><c r="G2" t="inlineStr"><is><t>1 µA</t></is></c></row><row r="3"><c r="A3" t="inlineStr"><is><t>OPA333AIDBVR-REEL</t></is></c><c r="B3" t="inlineStr"><is><t>OPA333</t></is></c><c r="C3" s="1"><v>201</v></c><c r="E3" t="inlineStr"><is><t>1.8 V</t></is></c><c r="F3" t="inlineStr"><is><t>5.5 V</t></is></c><c r="G3" t="inlineStr"><is><t>1 µA</t></is></c></row></sheetData></worksheet>)xml");
  return written && mz_zip_writer_finalize_archive(&archive) != 0 && mz_zip_writer_end(&archive) != 0;
}

int runCatalogueBenchmark(size_t requestedRows = 0) {
  using clock = chrono::steady_clock;
  const vector<size_t> sizes = {10000, 100000, 500000};
  for (const auto rows : sizes) {
    if (requestedRows != 0 && rows != requestedRows) continue;
    const auto databasePath = filesystem::temp_directory_path() / ("inventatory-catalogue-benchmark-" + to_string(rows) + ".db");
    const auto csvPath = filesystem::temp_directory_path() / ("TI_catalogue_benchmark_" + to_string(rows) + ".csv");
    error_code ignored;
    filesystem::remove(databasePath, ignored);
    filesystem::remove(csvPath, ignored);
    {
      ofstream csv(csvPath, ios::binary | ios::trunc);
      csv << "Orderable Part Number,Generic Part Number,Package,Channels,Supply voltage (min),Supply voltage (max),Shutdown current\n";
      for (size_t row = 0; row < rows; ++row) {
        const auto identifier = "BENCH" + to_string(row);
        csv << identifier << ",BASE" << row << ",0402,2,1.8 V,5.5 V,1 uA\n";
      }
    }
    {
      CatalogueDatabase database;
      if (!database.open(databasePath)) return 2;
      const auto importStart = clock::now();
      const auto imported = database.importFile(csvPath);
      const auto importElapsed = chrono::duration_cast<chrono::milliseconds>(clock::now() - importStart).count();
      if (!imported.error.empty() || imported.parts != rows) return 3;
      const auto lookupStart = clock::now();
      for (size_t lookup = 0; lookup < 100; ++lookup) {
        const auto result = database.lookup("Texas Instruments", "BENCH" + to_string((lookup * 7919) % rows));
        if (!result.matched()) return 4;
      }
      const auto lookupElapsed = chrono::duration_cast<chrono::microseconds>(clock::now() - lookupStart).count();
      cout << "catalogue benchmark rows=" << rows << " import_ms=" << importElapsed
           << " db_bytes=" << filesystem::file_size(databasePath)
           << " lookup_avg_us=" << (lookupElapsed / 100) << endl;
    }
    filesystem::remove(csvPath, ignored);
    filesystem::remove(databasePath, ignored);
    filesystem::remove(databasePath.string() + "-shm", ignored);
    filesystem::remove(databasePath.string() + "-wal", ignored);
  }
  return 0;
}

vector<InventoryItem> makeSampleInventory() {
  vector<InventoryItem> items;

  InventoryItem resistor;
  resistor.id = "res-0603-10k";
  resistor.partName = "10k Resistor 0603";
  resistor.manufacturer = "Yageo";
  resistor.category = "Resistors";
  resistor.quantity = 180;
  resistor.reorderThreshold = 50;
  resistor.location = "Shelf A3";
  resistor.tags = {"0603", "1%", "rohs"};
  resistor.parameters = {{"Resistance", "10k Ohm"}, {"Power", "0.1W"}, {"Package", "0603"}};
  resistor.notes = "General purpose pull-up and divider resistor.";
  resistor.manufacturerPartNumber = "RC0603FR-0710KL";
  resistor.datasheetUrl = "https://www.yageo.com/upload/media/product/productsearch/datasheet/rchip/PYu-RC_Group_51_RoHS_L_13.pdf";
  resistor.catalogueStatus = "not_in_catalogue";
  resistor.lastUpdated = 1710000000;
  items.push_back(resistor);

  InventoryItem module;
  module.id = "esp32-s3-module";
  module.partName = "ESP32-S3 Module";
  module.manufacturer = "Espressif";
  module.category = "MCUs";
  module.quantity = 4;
  module.reorderThreshold = 10;
  module.location = "ESD Drawer";
  module.tags = {"wifi", "bluetooth", "module"};
  module.parameters = {{"Core", "Xtensa LX7"}, {"Flash", "16MB"}, {"Package", "Module"}};
  module.notes = "Used for integration prototypes and test rigs.";
  module.manufacturerPartNumber = "ESP32-S3-WROOM-1";
  module.catalogueStatus = "not_in_catalogue";
  module.lastUpdated = 1710000100;
  items.push_back(module);

  ensureInventoryIdentifiers(items);
  return items;
}

class MockPrinterBackend final : public PrinterBackend {
 public:
  vector<PrinterQueueInfo> enumeratePrinters() const override {
    return printers_;
  }

  PrinterCheckResult probePrinter(const string& printerName) const override {
    if (printerName == configuredName_) {
      return {true, "Ready"};
    }
    return {false, "Queue not found"};
  }

  bool sendRawJob(const string& printerName, const string& jobName, const string& zpl, string* error) const override {
    lastPrinterName_ = printerName;
    lastJobName_ = jobName;
    lastZpl_ = zpl;
    if (printerName != configuredName_) {
      if (error != nullptr) {
        *error = "Wrong printer";
      }
      return false;
    }
    return true;
  }

  vector<PrinterQueueInfo> printers_ = {
      {"ZDesigner LP 2824 Plus (ZPL)", "ZDesigner", "USB001", "Ready", true, true},
      {"Microsoft Print to PDF", "Microsoft", "PORTPROMPT:", "Ready", false, true},
  };
  string configuredName_ = "ZDesigner LP 2824 Plus (ZPL)";
  mutable string lastPrinterName_;
  mutable string lastJobName_;
  mutable string lastZpl_;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && string(argv[1]) == "--catalogue-benchmark") {
    return runCatalogueBenchmark(argc == 3 ? stoull(argv[2]) : 0);
  }
  {
    auto items = makeSampleInventory();
    assert(!items.empty());

    const auto filtered = filterItems(items, "cat:resistors qty>100");
    assert(filtered.size() == 1);
    assert(items[filtered[0]].id == "res-0603-10k");

    // Malformed quantity filters must behave as a non-match rather than
    // propagating std::stoi exceptions through the interactive search path.
    assert(filterItems(items, "qty>not-a-number").empty());
    assert(filterItems(items, "qty>=999999999999999999999").empty());

    const auto tagFiltered = filterItems(items, "tag:module param:Flash=16MB");
    assert(tagFiltered.size() == 1);
    assert(items[tagFiltered[0]].id == "esp32-s3-module");
  }

  {
    auto items = makeSampleInventory();
    items[0].machineCode = "0002";
    items[1].machineCode = "0003";
    InventoryStore store;
    store.items() = items;

    const auto resolution = resolveScanCode(store, "RC0603FR-0710KL");
    assert(resolution.matched);
    assert(!resolution.created);
    assert(resolution.itemId == "res-0603-10k");

    const auto machineResolution = resolveScanCode(store, "0002");
    assert(machineResolution.matched);
    assert(!machineResolution.created);
    assert(machineResolution.itemId == "res-0603-10k");

    const auto rejected = resolveScanCode(store, "Inventatory:R-0002");
    assert(!rejected.matched);
    assert(rejected.message == "Unknown Inventatory ID");

    const auto unknownMachine = resolveScanCode(store, "9999");
    assert(!unknownMachine.matched);
    assert(unknownMachine.message == "Unknown machine code");

    const auto created = resolveScanCode(store, "new-supplier-code");
    assert(created.matched);
    assert(created.created);
    assert(store.findById(created.itemId) != nullptr);

    const auto unidentified = resolveDecodedComponent(store, "", "", "Unidentified sensor");
    assert(unidentified.matched);
    assert(unidentified.created);
    const auto* unidentifiedItem = store.findById(unidentified.itemId);
    assert(unidentifiedItem != nullptr);
    assert(unidentifiedItem->manufacturerPartNumber.empty());
    assert(unidentifiedItem->partName == "Unidentified sensor");
    assert(find(unidentifiedItem->tags.begin(), unidentifiedItem->tags.end(), "unidentifiable") !=
           unidentifiedItem->tags.end());
  }

  {
    vector<InventoryItem> items;
    InventoryItem resistor;
    resistor.id = "resistor-1";
    resistor.partName = "1k resistor";
    resistor.category = "Resistors";
    resistor.quantity = 10;
    resistor.lastUpdated = 1710000000;
    items.push_back(resistor);

    InventoryItem capacitor;
    capacitor.id = "capacitor-1";
    capacitor.partName = "10uF capacitor";
    capacitor.category = "Capacitors";
    capacitor.inventatoryId = "Inventatory:C-00127";
    capacitor.machineCode = "0002";
    capacitor.lastUpdated = 1710001000;
    items.push_back(capacitor);

    InventoryItem diode;
    diode.id = "diode-1";
    diode.partName = "Schottky diode";
    diode.category = "Diodes";
    diode.lastUpdated = 1710002000;
    items.push_back(diode);

    ensureInventoryIdentifiers(items);
    assert(isInventatoryId(items[0].inventatoryId));
    assert(items[0].inventatoryId.find("Inventatory:R-") == 0);
    assert(items[0].createdAt != 0);
    assert(items[1].inventatoryId == "Inventatory:C-00127");
    assert(items[0].machineCode == "0001");
    assert(items[1].machineCode == "0002");
    assert(items[2].machineCode == "0003");
    assert(buildVisibleInventatoryId(items[1]) == "Inventatory:C-0002");
  }

  {
    vector<InventoryItem> items;
    InventoryItem first;
    first.id = "dup-1";
    first.category = "Diodes";
    first.machineCode = "0002";
    first.lastUpdated = 1710003000;
    items.push_back(first);

    InventoryItem second;
    second.id = "dup-2";
    second.category = "Diodes";
    second.machineCode = "0002";
    second.lastUpdated = 1710004000;
    items.push_back(second);

    ensureInventoryIdentifiers(items);
    assert(items[0].machineCode != items[1].machineCode);
    assert(items[0].machineCode != "0002");
    assert(items[1].machineCode != "0002");
  }

  {
    InventoryStore rackStore;
    for (int index = 0; index < 26; ++index) {
      InventoryItem resistor;
      resistor.id = "rack-resistor-" + to_string(index);
      resistor.partName = to_string(index) + "k resistor";
      resistor.category = "Resistors";
      resistor.parameters = {{"Package", "0603"}};
      rackStore.items().push_back(resistor);
    }
    assert(reconcileRackAssignments(rackStore));
    assert(rackStore.racks().size() == 2);
    assert(rackStore.racks()[0].code == "R1");
    assert(rackLocation(rackStore.items()[0], rackStore.racks()) == "R1-A1");
    assert(rackLocation(rackStore.items()[24], rackStore.racks()) == "R1-E5");
    assert(rackLocation(rackStore.items()[25], rackStore.racks()) == "R2-A1");

    InventoryItem capacitor;
    capacitor.id = "rack-capacitor";
    capacitor.partName = "1uF capacitor";
    capacitor.category = "Capacitors";
    capacitor.parameters = {{"Package", "0805"}};
    rackStore.items().push_back(capacitor);
    reconcileRackAssignment(rackStore, rackStore.items().back());
    assert(rackStore.racks().size() == 3);
    assert(rackLocation(rackStore.items().back(), rackStore.racks()) == "R3-A1");
    assert(rackOccupiedSlotCount(rackStore, rackStore.racks()[0]) == 25);
    assert(rackOccupiedSlotCount(rackStore, rackStore.racks()[1]) == 1);
    assert(itemAtRackSlot(rackStore, rackStore.racks()[0].id, "A1") == &rackStore.items()[0]);
    assert(itemAtRackSlot(rackStore, rackStore.racks()[0].id, "E5") == &rackStore.items()[24]);
    assert(itemAtRackSlot(rackStore, rackStore.racks()[0].id, "B5") == &rackStore.items()[9]);
    assert(itemAtRackSlot(rackStore, rackStore.racks()[1].id, "E5") == nullptr);
    assert(rackSlotLabel(0, 0) == "A1");
    assert(rackSlotLabel(4, 4) == "E5");
    assert(rackSlotLabel(5, 0).empty());

    string moveError;
    assert(!moveItemToRackSlot(rackStore, rackStore.items()[0], rackStore.racks()[0], "A2", moveError));
    assert(moveError.find("occupied") != string::npos);
    assert(moveItemToRackSlot(rackStore, rackStore.items()[0], rackStore.racks()[1], "E5", moveError));
    assert(rackStore.items()[0].rackAssignment == RackAssignmentMode::Manual);
    assert(rackLocation(rackStore.items()[0], rackStore.racks()) == "R2-E5");
    assert(itemAtRackSlot(rackStore, rackStore.racks()[1].id, "E5") == &rackStore.items()[0]);
    assert(rackOccupiedSlotCount(rackStore, rackStore.racks()[0]) == 24);
    assert(rackOccupiedSlotCount(rackStore, rackStore.racks()[1]) == 2);
    assert(unassignItemFromRack(rackStore.items()[0]));
    reconcileRackAssignment(rackStore, rackStore.items()[0]);
    assert(rackStore.items()[0].rackAssignment == RackAssignmentMode::Unassigned);
    assert(rackLocation(rackStore.items()[0], rackStore.racks()).empty());
    restoreAutomaticRackAssignment(rackStore, rackStore.items()[0]);
    assert(rackStore.items()[0].rackAssignment == RackAssignmentMode::Manual);
    assert(rackLocation(rackStore.items()[0], rackStore.racks()) == "R1-A1");
    rackStore.items()[0].category = "Capacitors";
    reconcileRackAssignment(rackStore, rackStore.items()[0]);
    assert(rackLocation(rackStore.items()[0], rackStore.racks()) == "R1-A1");

    InventoryItem module;
    module.id = "rack-module";
    module.partName = "ESP32-S3 Module";
    module.category = "MCUs";
    module.parameters = {{"Package", "Module"}};
    rackStore.items().push_back(module);
    reconcileRackAssignment(rackStore, rackStore.items().back());
    assert(rackStore.items().back().rackAssignment == RackAssignmentMode::Unassigned);
    assert(rackLocation(rackStore.items().back(), rackStore.racks()).empty());

    InventoryItem radialCapacitor;
    radialCapacitor.id = "rack-radial-capacitor";
    radialCapacitor.partName = "Aluminum electrolytic capacitor";
    radialCapacitor.category = "Capacitors";
    radialCapacitor.parameters = {{"Package / Case", "Radial, Can"}, {"Mounting Type", "Through Hole"}};
    rackStore.items().push_back(radialCapacitor);
    reconcileRackAssignment(rackStore, rackStore.items().back());
    assert(rackStore.items().back().rackAssignment == RackAssignmentMode::Unassigned);
    assert(rackLocation(rackStore.items().back(), rackStore.racks()).empty());

    InventoryItem powerMosfet;
    powerMosfet.id = "rack-power-mosfet";
    powerMosfet.partName = "Power MOSFET";
    powerMosfet.category = "MOSFETs";
    powerMosfet.parameters = {{"Package / Case", "TO-263-3, D2PAK"}};
    rackStore.items().push_back(powerMosfet);
    reconcileRackAssignment(rackStore, rackStore.items().back());
    assert(rackStore.items().back().rackAssignment == RackAssignmentMode::Manual);
    assert(rackStore.racks().size() == 4);
    assert(rackStore.racks()[3].componentType == "Transistors");
    assert(rackLocation(rackStore.items().back(), rackStore.racks()) == "R4-A1");

    InventoryItem compactMosfet;
    compactMosfet.id = "rack-compact-mosfet";
    compactMosfet.partName = "Small MOSFET";
    compactMosfet.category = "MOSFETs";
    compactMosfet.parameters = {{"Package / Case", "SOT-23"}};
    rackStore.items().push_back(compactMosfet);
    reconcileRackAssignment(rackStore, rackStore.items().back());
    assert(rackLocation(rackStore.items().back(), rackStore.racks()) == "R4-A2");

    {
      InventoryStore autoStore;
      InventoryItem buckIc;
      buckIc.id = "rack-buck-ic";
      buckIc.partName = "Buck converter";
      buckIc.category = "Integrated Circuits";
      buckIc.notes = "Integrated circuit with diode clamp.";
      buckIc.parameters = {{"Package / Case", "QFN-16"}, {"Function", "DC-DC converter"}};
      autoStore.items().push_back(buckIc);
      reconcileRackAssignment(autoStore, autoStore.items().back());
      assert(autoStore.racks().size() == 1);
      assert(autoStore.racks()[0].componentType == "Integrated Circuits");
      assert(rackLocation(autoStore.items().back(), autoStore.racks()) == "R1-A1");
    }

    {
      InventoryStore autoStore;
      InventoryItem fuse;
      fuse.id = "rack-fuse";
      fuse.partName = "Resettable fuse";
      fuse.category = "Fuses";
      fuse.parameters = {{"Package / Case", "1206"}};
      autoStore.items().push_back(fuse);
      reconcileRackAssignment(autoStore, autoStore.items().back());
      assert(autoStore.racks().size() == 1);
      assert(autoStore.racks()[0].componentType == "Fuses");
      assert(rackLocation(autoStore.items().back(), autoStore.racks()) == "R1-A1");
    }

    {
      InventoryStore autoStore;
      InventoryItem connector;
      connector.id = "rack-connector";
      connector.partName = "Board connector";
      connector.category = "Connectors";
      connector.parameters = {{"Package / Case", "Through Hole"}};
      autoStore.items().push_back(connector);
      reconcileRackAssignment(autoStore, autoStore.items().back());
      assert(autoStore.racks().size() == 1);
      assert(autoStore.racks()[0].componentType == "Connectors");
      assert(rackLocation(autoStore.items().back(), autoStore.racks()) == "R1-A1");
    }

    {
      InventoryStore legacyStore;
      InventatoryRack legacyRack;
      legacyRack.id = "legacy-rack";
      legacyRack.code = "R9";
      legacyRack.componentType = "integrated-circuits";
      legacyStore.racks().push_back(legacyRack);
      InventoryItem legacyIc;
      legacyIc.id = "legacy-ic";
      legacyIc.partName = "Buck regulator";
      legacyIc.category = "Integrated Circuits";
      legacyIc.parameters = {{"Package / Case", "QFN-16"}};
      legacyStore.items().push_back(legacyIc);
      reconcileRackAssignment(legacyStore, legacyStore.items().back());
      assert(legacyStore.racks().size() == 1);
      assert(rackLocation(legacyStore.items().back(), legacyStore.racks()) == "R9-A1");
    }

    string error;
    auto& manuallyPlaced = rackStore.items()[26];
    assert(setManualRackLocation(rackStore, manuallyPlaced, "R2-E5", error));
    assert(manuallyPlaced.rackAssignment == RackAssignmentMode::Manual);
    assert(rackLocation(manuallyPlaced, rackStore.racks()) == "R2-E5");
    manuallyPlaced.category = "Integrated Circuits";
    manuallyPlaced.parameters = {{"Package", "QFN-16"}};
    reconcileRackAssignment(rackStore, manuallyPlaced);
    assert(rackLocation(manuallyPlaced, rackStore.racks()) == "R2-E5");

    assert(!setManualRackLocation(rackStore, manuallyPlaced, "R2-A1", error));
    assert(error.find("occupied") != string::npos);
    assert(!setManualRackLocation(rackStore, manuallyPlaced, "R99-A1", error));
    assert(error.find("does not exist") != string::npos);
    assert(!setManualRackLocation(rackStore, manuallyPlaced, "R2-Z9", error));
    assert(error.find("A1 through E5") != string::npos);

    assert(setManualRackLocation(rackStore, manuallyPlaced, "", error));
    reconcileRackAssignment(rackStore, manuallyPlaced);
    assert(manuallyPlaced.rackAssignment == RackAssignmentMode::Unassigned);
    assert(rackLocation(manuallyPlaced, rackStore.racks()).empty());
    assert(setManualRackLocation(rackStore, manuallyPlaced, "AUTO", error));
    reconcileRackAssignment(rackStore, manuallyPlaced);
    assert(manuallyPlaced.rackAssignment == RackAssignmentMode::Manual);
    assert(!rackLocation(manuallyPlaced, rackStore.racks()).empty());
    const auto automaticLocation = rackLocation(manuallyPlaced, rackStore.racks());
    assert(matchesQuery(manuallyPlaced, "rack:" + automaticLocation, rackStore.racks()));
    assert(matchesQuery(manuallyPlaced, automaticLocation, rackStore.racks()));
    const auto stockFields = stockPreviewFields(manuallyPlaced, automaticLocation);
    assert(!stockFields.empty());
    assert(stockFields.front().label == "Inventatory RACK: ");
    assert(stockFields.front().value == automaticLocation);
    bool foundAtAGlance = false;
    for (const auto& field : stockFields) {
      if (field.label == "At a glance: ") {
        foundAtAGlance = true;
        assert(!field.value.empty());
      }
    }
    assert(foundAtAGlance);
    const auto coreFields = detailCoreFields(manuallyPlaced, automaticLocation);
    assert(!coreFields.empty());
    assert(coreFields.front().value == automaticLocation);

    const auto tempPath = filesystem::temp_directory_path() / "inventatory-rack-roundtrip.db";
    assert(rackStore.save(tempPath));
    InventoryStore loadedRackStore;
    assert(loadedRackStore.load(tempPath));
    assert(loadedRackStore.racks().size() == rackStore.racks().size());
    const auto* loadedCapacitor = loadedRackStore.findById("rack-capacitor");
    assert(loadedCapacitor != nullptr);
    assert(!rackLocation(*loadedCapacitor, loadedRackStore.racks()).empty());
    filesystem::remove(tempPath);
  }

  {
    InventoryItem item;
    item.id = "abc";
    item.partName = "Test Part";
    item.manufacturer = "Acme";
    item.category = "Test";
    item.quantity = 2;
    item.reorderThreshold = 1;
    item.location = "Bin 1";
    item.tags = {"alpha|beta", R"(path\\value)"};
    item.parameters = {{"Voltage=nominal", "5V; tolerance=1%"}, {"Package", R"(0805\\metric)"}};
    item.notes = "Roundtrip test";
    item.manufacturerPartNumber = "123";
    item.datasheetUrl = "https://example.com/datasheet";
    item.datasheetUrl = "https://example.com/product";
    item.catalogueStatus = "matched";
    item.manufacturerPartNumber = "SKU-1";
    item.lastUpdated = 1710000000;
    item.machineCode = "0002";

    const auto line = serializeItem(item);
    InventoryItem restored;
    assert(deserializeItem(line, restored));
    assert(restored.partName == item.partName);
    assert(restored.parameters.size() == 2);
    assert(restored.tags.size() == 2);
    assert(restored.tags == item.tags);
    assert(restored.parameters[0].name == item.parameters[0].name);
    assert(restored.parameters[0].value == item.parameters[0].value);
    assert(restored.inventatoryId == item.inventatoryId);
    assert(restored.machineCode == item.machineCode);
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-catalogue-migration-test.db";
    filesystem::remove(path);
    {
      SqliteConnection connection;
      assert(openDatabase(path, connection));
      assert(execSql(connection, R"SQL(
        CREATE TABLE inventatory_items (
          id TEXT PRIMARY KEY, part_name TEXT NOT NULL, manufacturer TEXT NOT NULL, category TEXT NOT NULL,
          quantity INTEGER NOT NULL, reorder_threshold INTEGER NOT NULL, location TEXT NOT NULL,
          tags TEXT NOT NULL, parameters TEXT NOT NULL, notes TEXT NOT NULL,
          supplier_code TEXT NOT NULL, datasheet_url TEXT NOT NULL, product_url TEXT NOT NULL,
          sync_state TEXT NOT NULL, sku TEXT NOT NULL, last_updated INTEGER NOT NULL,
          inventatory_id TEXT NOT NULL, created_at INTEGER NOT NULL, machine_code TEXT NOT NULL,
          rack_id TEXT NOT NULL, rack_slot TEXT NOT NULL, rack_assignment TEXT NOT NULL
        );
        INSERT INTO inventatory_items VALUES
          ('legacy-pdf','Legacy PDF','Maker','ICs',7,2,'A1','','','keep note','drop-me',
           'https://maker.example/direct.pdf','https://catalog.example/product','synced','MPN-1',100,
           'Inventatory:I-1',90,'0002','','','automatic'),
          ('legacy-page','Legacy Page','Maker','ICs',3,1,'A2','','','','drop-me-too',
           'https://catalog.example/product','https://catalog.example/search','synced','MPN-2',101,
           'Inventatory:I-2',91,'0003','','','automatic');
      )SQL"));
    }
    InventoryStore migrated;
    assert(migrated.load(path));
    assert(migrated.items().size() == 2);
    assert(migrated.findById("legacy-pdf")->manufacturerPartNumber == "MPN-1");
    assert(migrated.findById("legacy-pdf")->datasheetUrl == "https://maker.example/direct.pdf");
    assert(migrated.findById("legacy-pdf")->notes == "keep note");
    assert(migrated.findById("legacy-page")->datasheetUrl.empty());
    assert(migrated.findById("legacy-page")->quantity == 3);
    assert(migrated.findById("legacy-page")->catalogueStatus == "not_in_catalogue");
    {
      SqliteConnection connection;
      assert(openDatabase(path, connection));
      assert(tableColumnExists(connection, "inventatory_items", "manufacturer_part_number"));
      assert(!tableColumnExists(connection, "inventatory_items", "supplier_code"));
      assert(!tableColumnExists(connection, "inventatory_items", "product_url"));
      assert(!tableColumnExists(connection, "inventatory_items", "sync_state"));
      assert(!tableColumnExists(connection, "inventatory_items", "sku"));
    }
    filesystem::remove(path);
  }

  {
    const auto tempPath = filesystem::temp_directory_path() / "inventatory-machine-code-roundtrip.db";
    InventoryStore store;
    InventoryItem item;
    item.id = "roundtrip-1";
    item.partName = "Roundtrip part";
    item.manufacturer = "Acme";
    item.category = "Diodes";
    item.quantity = 1;
    item.lastUpdated = 1710000000;
    item.machineCode = "0002";
    item.tags = {"lab|bench", R"(path\fixture)"};
    item.parameters = {{"Test=Point", "A;B=C"}};
    store.items().push_back(item);

    assert(store.save(tempPath));
    InventoryStore loaded;
    assert(loaded.load(tempPath));
    assert(!loaded.items().empty());
    assert(loaded.items().front().machineCode == "0002");
    assert(loaded.items().front().tags == item.tags);
    assert(loaded.items().front().parameters.size() == 1);
    assert(loaded.items().front().parameters.front().name == "Test=Point");
    assert(loaded.items().front().parameters.front().value == "A;B=C");
    filesystem::remove(tempPath);
  }

  {
    const string csv =
        "Manufacturer Part Number,Manufacturer,Part Name,Quantity,Datasheet URL,Location,Notes,Tags\n"
        "CDMC6D28NP-4R7MC,Sumida,Shielded power inductor,10,https://example.test/part.pdf,Drawer A,For power rails,power;inductor\n";
    const auto result = parseBomCsvText(csv, {});
    assert(result.ok);
    assert(result.candidates.size() == 1);
    const auto& candidate = result.candidates.front();
    assert(candidate.item.manufacturerPartNumber == "CDMC6D28NP-4R7MC");
    assert(candidate.item.manufacturer == "Sumida");
    assert(candidate.item.quantity == 10);
    assert(candidate.item.datasheetUrl == "https://example.test/part.pdf");
    assert(candidate.item.tags.size() == 3);
  }

  {
    const string csv =
        "Manufacturer Part Number,Manufacturer,Description,Quantity\n"
        "C0603C105K4RACTU,KEMET,CAP CER 1UF 16V X7R 0603,50\n";
    auto existing = makeSampleInventory();
    InventoryItem duplicate;
    duplicate.id = "existing-cap";
    duplicate.partName = "Existing Cap";
    duplicate.manufacturer = "KEMET";
    duplicate.category = "Capacitors";
    duplicate.quantity = 7;
    duplicate.manufacturerPartNumber = "C0603C105K4RACTU";
    existing.push_back(duplicate);
    const auto result = parseBomCsvText(csv, existing);
    assert(result.ok);
    assert(result.candidates.size() == 1);
    assert(result.candidates.front().hasConflict);
    assert(result.candidates.front().existingItemId == "existing-cap");
    assert(result.candidates.front().matchedField == "Manufacturer and part number");
    mergeImportedMetadata(duplicate, result.candidates.front().item);
    duplicate.quantity += result.candidates.front().item.quantity;
    assert(duplicate.quantity == 57);
  }

  {
    const string csv = "Manufacturer Part Number,Quantity\nSHARED-1,4\n";
    InventoryItem first;
    first.id = "maker-a-shared";
    first.partName = "Maker A part";
    first.manufacturer = "Maker A";
    first.manufacturerPartNumber = "SHARED-1";
    InventoryItem second = first;
    second.id = "maker-b-shared";
    second.partName = "Maker B part";
    second.manufacturer = "Maker B";
    const auto result = parseBomCsvText(csv, {first, second});
    assert(result.ok);
    assert(result.candidates.front().hasConflict);
    assert(result.candidates.front().existingItemId.empty());
    assert(result.candidates.front().matchedField.find("Ambiguous") != string::npos);
  }

  {
    auto backend = make_unique<MockPrinterBackend>();
    auto* backendPtr = backend.get();
    LabelPrinterService service(move(backend));
    service.setConfiguredPrinter("ZDesigner LP 2824 Plus (ZPL)");
    const auto expectHeader = [&](InventoryItem& candidate, const string& expected) {
      candidate.cataloguePurposeLabel = expected;
      candidate.cataloguePrintLabel = expected.size() <= 16 ? expected : expected.substr(0, 16);
      candidate.catalogueStatus = "matched";
      const auto plan = service.buildLabelPlan(candidate);
      const auto expectedHeader = candidate.cataloguePrintLabel;
      if (plan.categoryHeader != expectedHeader) {
        cerr << "Expected header '" << expectedHeader << "' but got '" << plan.categoryHeader << "' for "
             << candidate.id << '\n';
      }
      assert(plan.categoryHeader == expectedHeader);
      assert(service.buildZpl(candidate).find("^FD" + expectedHeader + "^FS") != string::npos);
      return plan;
    };

    struct DescriptorGoldenCase {
      string id;
      string partName;
      string manufacturer;
      string category;
      vector<Parameter> parameters;
      string notes;
      string expected;
    };

    const vector<DescriptorGoldenCase> descriptorGoldenCases = {
        {"golden-real-ne555", "IC OSC SNGL TIMER 100KHZ 8-SOIC", "Texas Instruments", "Integrated Circuits (ICs)",
         {{"Type", "Surface Mount"},
          {"Count", "-"},
          {"Frequency", "100kHz"},
          {"Voltage - Supply", "4.5V ~ 16V"},
          {"Operating Temperature", "0C ~ 70C"},
          {"Package / Case", "8-SOIC"}},
         "Created from a supplier code scan.", "Timer IC"},
        {"golden-voltage-ref", "Precision voltage reference", "Microchip", "Integrated Circuits",
         {{"Voltage Reference Type", "Shunt"}, {"Package / Case", "SOT-23"}}, {}, "Voltage Ref."},
        {"golden-buck", "IC REG BUCK 3.3V 2A TSOT23-6", "Monolithic Power", "Integrated Circuits",
         {{"Function", "DC-DC converter"}, {"Topology", "Buck"}, {"Package / Case", "TSOT-23-6"}}, {}, "Buck Converter"},
        {"golden-boost", "Boost converter", "Analog Devices", "Power Management ICs",
         {{"Function", "DC-DC converter"}, {"Topology", "Boost"}}, {}, "Boost Converter"},
        {"golden-buck-boost", "Buck-boost converter", "Texas Instruments", "Power Management ICs",
         {{"Function", "DC-DC converter"}, {"Topology", "Buck-Boost"}}, {}, "Buck-Boost Conv."},
        {"golden-ldo", "Low dropout linear regulator", "Microchip", "Voltage Regulators",
         {{"Output Voltage", "3.3V"}, {"Type", "LDO"}}, {}, "Regulator"},
        {"golden-charger", "Li-ion battery charger", "Microchip", "Power Management ICs",
         {{"Function", "Battery Charger"}}, {}, "Battery Charger"},
        {"golden-load-switch", "Power distribution load switch", "Texas Instruments", "Power Management ICs",
         {{"Function", "Load Switch"}}, {}, "Load Switch"},
        {"golden-supervisor", "Voltage supervisor reset IC", "onsemi", "Power Management ICs",
         {{"Type", "Voltage Supervisor"}}, {}, "Voltage Supervisor"},
        {"golden-protection", "ESD protection array", "Nexperia", "Integrated Circuits",
         {{"Function", "Protection"}, {"Type", "ESD"}}, {}, "Protection IC"},
        {"golden-opamp", "Dual operational amplifier", "Texas Instruments", "Linear - Amplifiers",
         {{"Gain Bandwidth", "10MHz"}, {"Slew Rate", "5V/us"}}, {}, "OP-AMP"},
        {"golden-comparator", "Dual comparator", "onsemi", "Linear - Comparators",
         {{"Function", "Comparator"}}, {}, "Comparator"},
        {"golden-adc", "12-bit analog to digital converter", "Microchip", "Data Acquisition",
         {{"Resolution", "12 bit"}, {"Interface", "SPI"}}, {}, "ADC"},
        {"golden-dac", "Digital to analog converter", "Microchip", "Data Acquisition",
         {{"Resolution", "12 bit"}, {"Interface", "I2C"}}, {}, "DAC"},
        {"golden-rtc", "Real-time clock IC", "NXP", "Clock/Timing",
         {{"Interface", "I2C"}}, {}, "RTC"},
        {"golden-clock-gen", "Clock generator PLL", "Skyworks", "Clock/Timing",
         {{"Frequency", "100MHz"}}, {}, "Clock Gen."},
        {"golden-memory", "Serial EEPROM memory", "Microchip", "Memory",
         {{"Memory Type", "EEPROM"}, {"Memory Size", "256Kbit"}, {"Memory Interface", "I2C"}}, {}, "Memory IC"},
        {"golden-logic-gate", "Quad NAND gate", "Texas Instruments", "Logic",
         {{"Function", "Logic Gate"}}, {}, "Logic Gate"},
        {"golden-logic-buffer", "Tri-state bus buffer", "Nexperia", "Logic",
         {{"Function", "Logic Buffer"}}, {}, "Logic Buffer"},
        {"golden-level-shifter", "Voltage level shifter", "Nexperia", "Logic",
         {{"Function", "Voltage Translator"}}, {}, "Level Shifter"},
        {"golden-transceiver", "CAN bus transceiver", "Texas Instruments", "Interface",
         {{"Interface", "CAN"}}, {}, "Transceiver"},
        {"golden-mux", "Analog multiplexer demultiplexer", "Analog Devices", "Interface",
         {{"Function", "Multiplexer"}}, {}, "Mux/Demux"},
        {"golden-hbridge", "H-bridge motor driver", "STMicroelectronics", "Integrated Circuits",
         {{"Function", "Motor control"}, {"Output Type", "H-Bridge"}}, {}, "H-Bridge"},
        {"golden-motor-driver", "Dual motor driver", "Texas Instruments", "Integrated Circuits",
         {{"Function", "Motor Driver"}, {"Output Type", "Half Bridge"}}, {}, "Motor Driver"},
        {"golden-gate-driver", "MOSFET gate driver", "Microchip", "Integrated Circuits",
         {{"Function", "Gate Driver"}}, {}, "Gate Driver"},
        {"golden-temp-sensor", "Digital temperature sensor", "Texas Instruments", "Sensors",
         {{"Sensor Type", "Temperature"}, {"Output", "I2C"}}, {}, "Temp Sensor"},
        {"golden-pressure-sensor", "Board mount pressure sensor", "Bosch", "Sensors",
         {{"Sensor Type", "Pressure"}, {"Output", "I2C"}}, {}, "Pressure Sensor"},
        {"golden-imu", "3-axis IMU", "TDK InvenSense", "Sensors",
         {{"Sensor Type", "3-axis accelerometer / gyroscope"}, {"Output", "I2C"}}, {}, "3 Axis IMU"},
        {"golden-current-sensor", "Current monitor sensor", "Allegro", "Sensors",
         {{"Function", "Current Monitor"}, {"Output", "Analog"}}, {}, "Current Sensor"},
        {"golden-mcu", "STM32G0 microcontroller", "STMicroelectronics", "MCUs",
         {{"Core Processor", "ARM Cortex-M0+"}, {"Program Memory Size", "128KB"}}, {}, "MCU"},
        {"golden-mosfet", "N-channel MOSFET", "Alpha & Omega", "MOSFETs",
         {{"Drain-Source Voltage", "30V"}, {"Rds On", "12mOhm"}}, {}, "MOSFET"},
        {"golden-bjt", "NPN transistor", "onsemi", "Transistors",
         {{"Collector Current", "600mA"}}, {}, "Transistor"},
        {"golden-tvs", "ESD TVS diode", "Littelfuse", "Transient Voltage Suppressors",
         {{"Voltage - Reverse Standoff (Typ)", "16V"}}, {}, "TVS Diode"},
        {"golden-schottky", "Schottky rectifier diode", "Diodes Inc.", "Schottky Diodes",
         {{"Forward Voltage", "0.38V"}, {"Reverse Voltage", "40V"}}, {}, "Schottky Diode"},
        {"golden-connector", "Board header connector", "Harwin", "Connectors",
         {{"Number of Positions", "8"}, {"Connector Type", "Header"}}, {}, "Connector"},
        {"golden-resistor", "RES 10K OHM 1% 0603", "Yageo", "Resistors",
         {{"Resistance", "10k Ohm"}, {"Tolerance", "1%"}}, {}, "Resistor"},
        {"golden-capacitor", "CAP CER 1UF 16V X7R 0603", "Murata", "Capacitors",
         {{"Capacitance", "1uF"}, {"Voltage - Rated", "16V"}}, {}, "Capacitor"},
        {"golden-inductor", "FIXED IND 4.7UH 3.7A", "Sumida", "Inductors",
         {{"Inductance", "4.7uH"}, {"Current Rating", "3.7A"}}, {}, "Inductor"},
        {"golden-crystal", "16MHz crystal", "Abracon", "Crystals",
         {{"Frequency", "16MHz"}, {"Load Capacitance", "18pF"}}, {}, "Crystal"},
    };

    for (const auto& golden : descriptorGoldenCases) {
      InventoryItem candidate;
      candidate.id = golden.id;
      candidate.partName = golden.partName;
      candidate.manufacturer = golden.manufacturer;
      candidate.category = golden.category;
      candidate.parameters = golden.parameters;
      candidate.notes = golden.notes;
      expectHeader(candidate, golden.expected);
      assert(partShortDescription(candidate) == golden.expected);
    }

    const auto info = service.configuredPrinterInfo();
    assert(info.has_value());
    assert(info->portName == "USB001");

    const auto check = service.probeConfiguredPrinter();
    assert(check.ok);

    InventoryItem item;
    item.id = "res-0603-10k";
    item.partName = "10k resistor";
    item.manufacturer = "Yageo";
    item.category = "Resistors";
    item.quantity = 100;
    item.reorderThreshold = 20;
    item.lastUpdated = 1710000000;
    item.createdAt = 1710000000;
    item.inventatoryId = "Inventatory:R-00123";
    item.machineCode = "0002";
    item.manufacturerPartNumber = "RC0603FR-0710KL";
    item.cataloguePurposeLabel = "Resistor";
    item.cataloguePrintLabel = "Resistor";
    item.catalogueStatus = "matched";
    item.parameters = {{"Resistance", "10k Ohm"}, {"Tolerance", "1%"}, {"Power Dissipation", "0.125W"}};

    const auto plan = service.buildLabelPlan(item);
    assert(plan.categoryHeader == "Resistor");
    assert(plan.mainValue.find("10k") != string::npos);
    assert(plan.mainValue.find(u8"\u03A9") != string::npos);
    assert(plan.packageLine.empty());
    assert(plan.manufacturerLine == "Yageo");
    assert(plan.parameterLine1.find("R ") != string::npos);
    assert(plan.parameterLine1.find("10k") != string::npos);
    assert(plan.parameterLine2.find("Pwr") != string::npos);
    assert(plan.parameterLine3.empty());
    assert(plan.inventatoryId == "Inventatory:R-00123");
    assert(plan.scannerHint == "Inventatory:R-0002");
    assert(plan.barcodeHint == "0002");
    const auto zpl = service.buildZpl(item);
    assert(!zpl.empty());
    assert(zpl.find("10kOhms") == string::npos);
    assert(zpl.find("Tol ") == string::npos);
    assert(zpl.find("Tempco") == string::npos);
    assert(zpl.find("^FDResistor^FS") != string::npos);
    assert(zpl.find("^FDPwr 0.125W^FS") != string::npos);
    assert(zpl.find("^FO10,100^A0N,16,16^FDYageo^FS") != string::npos);
    assert(zpl.find("^BQN,2,3") != string::npos);
    const auto rackPlan = service.buildLabelPlan(item, "R3-E3");
    assert(rackPlan.rackLocation == "R3-E3");
    const auto rackZpl = service.buildZpl(item, "R3-E3");
    assert(rackZpl.find("^FO10,173^A0N,13,13^FDR3-E3^FS") != string::npos);
    assert(zpl.find("^FDLA,0002^FS") != string::npos);
    assert(zpl.find("^FO200,6^A0N,17,17^FR^FDInventatory^FS") == string::npos);
    assert(zpl.find("^FDInventatory:R-0002^FS") == string::npos);
    assert(zpl.find("^FO166,136^A0N,10,10^FB76,1,0,C^FDR-0002\\&^FS") != string::npos);
    assert(zpl.find("^FO5,0^GB251,24,24,B,6^FS") != string::npos);
    assert(zpl.find("^FO171,190^A0N,8,8^FB80,1,0,R^FDInventatory\\&^FS") != string::npos);
    assert(zpl.find("^BC") == string::npos);

    string error;
    assert(service.printItemLabel(item, &error));
    assert(error.empty());
    assert(backendPtr->lastPrinterName_ == "ZDesigner LP 2824 Plus (ZPL)");
    assert(backendPtr->lastJobName_.find("Inventatory Label") == 0);
    assert(backendPtr->lastZpl_.find("^FDLA,0002^FS") != string::npos);
    const auto wireZpl = service.buildWireLabelZpl("12V");
    assert(wireZpl.find("^A0N,68,58^FB236,1,0,C^FD12V^FS") != string::npos);
    assert(wireZpl.find("^FO10,112^A0N,68,58^FB236,1,0,C^FD12V^FS") != string::npos);
    const auto longWireZpl = service.buildWireLabelZpl("CONTROL SIGNAL +5V");
    assert(longWireZpl.find("^A0N,28,16^FB236,1,0,C^FDCONTROL SIGNAL +5V^FS") != string::npos);
    assert(longWireZpl.find("^FO10,112^A0N,28,16^FB236,1,0,C^FDCONTROL SIGNAL +5V^FS") != string::npos);
    assert(service.printWireLabel("GND", &error));
    assert(backendPtr->lastJobName_ == "Inventatory Wire Label");

    InventatoryRack resistorRack;
    resistorRack.id = "rack-res-1";
    resistorRack.code = "R1";
    resistorRack.componentType = "resistors";
    const auto rackLabelPlan = service.buildRackLabelPlan(resistorRack);
    assert(rackLabelPlan.categoryText == "RESISTORS");
    assert(rackLabelPlan.rackText == "RACK 01");
    const auto rackLabelZpl = service.buildRackLabelZpl(resistorRack);
    assert(rackLabelZpl.find("^FDInventatory RACK^FS") != string::npos);
    assert(rackLabelZpl.find("^FDRESISTORS^FS") != string::npos);
    assert(rackLabelZpl.find("^FDRACK 01^FS") != string::npos);
    assert(rackLabelZpl.find("^GFA") == string::npos);
    assert(rackLabelZpl.find("^FO6,55^A0N,40,34^FB244,1,0,C^FDRESISTORS^FS") != string::npos);

    InventatoryRack customRack;
    customRack.id = "rack-custom-1";
    customRack.code = "R12";
    customRack.componentType = "smd widgets";
    const auto customRackPlan = service.buildRackLabelPlan(customRack);
    assert(customRackPlan.categoryText == "SMD WIDGETS");
    assert(customRackPlan.rackText == "RACK 12");
    const auto customRackZpl = service.buildRackLabelZpl(customRack);
    assert(customRackZpl.find("^FO6,48^A0N,27,24^FB244,2,4,C^FDSMD\\&WIDGETS^FS") != string::npos);
    assert(customRackZpl.find("^GFA") == string::npos);

    InventatoryRack longRack;
    longRack.id = "rack-long-1";
    longRack.code = "R13";
    longRack.componentType = "integrated circuits";
    const auto longRackZpl = service.buildRackLabelZpl(longRack);
    assert(longRackZpl.find("^FDINTEGRATED\\&CIRCUITS^FS") != string::npos);

    error.clear();
    assert(service.printRackLabel(resistorRack, &error));
    assert(error.empty());
    assert(backendPtr->lastJobName_ == "Inventatory Rack R1");
    assert(backendPtr->lastZpl_.find("^FDRACK 01^FS") != string::npos);

    InventoryItem tvsDiode;
    tvsDiode.id = "tvs-1";
    tvsDiode.partName = "ESD clamp";
    tvsDiode.manufacturer = "Littelfuse";
    tvsDiode.category = "Transient Voltage Suppressors";
    tvsDiode.parameters = {{"Voltage - Reverse Standoff (Typ)", "16V"},
                           {"Voltage - Clamping (Max) @ Ipp", "26V"},
                           {"Current - Peak Pulse (10/1000µs)", "23.1A"},
                           {"Power - Peak Pulse", "600W"}};
    const auto tvsPlan = expectHeader(tvsDiode, "TVS Diode");
    assert(tvsPlan.categoryHeader == "TVS Diode");
    assert(service.buildZpl(tvsDiode).find("^FDTVS Diode^FS") != string::npos);

    InventoryItem protectionIc;
    protectionIc.id = "prot-ic-1";
    protectionIc.partName = "ESD protection array";
    protectionIc.manufacturer = "Nexperia";
    protectionIc.category = "Integrated Circuits";
    protectionIc.parameters = {{"Function", "Protection"}, {"Type", "ESD"}};
    const auto protectionPlan = expectHeader(protectionIc, "Protection IC");
    assert(protectionPlan.categoryHeader == "Protection IC");
    assert(service.buildZpl(protectionIc).find("^FDProtection IC^FS") != string::npos);

    {
      InventoryItem buckIc;
      buckIc.id = "buck-ic-1";
      buckIc.partName = "Synchronous buck converter";
      buckIc.manufacturer = "Texas Instruments";
      buckIc.category = "Integrated Circuits";
      buckIc.notes = "Integrated circuit with diode clamp and switching regulator topology from a wide input rail.";
      buckIc.parameters = {{"Function", "DC-DC converter"}, {"Topology", "Buck"}, {"Package / Case", "QFN-16"}};
      const auto buckPlan = expectHeader(buckIc, "Buck Converter");
      assert(buckPlan.mainValue.find("buck") != string::npos || buckPlan.mainValue.find("converter") != string::npos);
    }

    {
      InventoryItem boostIc;
      boostIc.id = "boost-ic-1";
      boostIc.partName = "Boost converter";
      boostIc.manufacturer = "Analog Devices";
      boostIc.category = "Integrated Circuits";
      boostIc.parameters = {{"Function", "DC-DC converter"}, {"Topology", "Boost"}, {"Package / Case", "QFN-20"}};
      expectHeader(boostIc, "Boost Converter");
    }

    {
      InventoryItem buckBoostIc;
      buckBoostIc.id = "buck-boost-ic-1";
      buckBoostIc.partName = "Buck-boost converter";
      buckBoostIc.manufacturer = "Texas Instruments";
      buckBoostIc.category = "Integrated Circuits";
      buckBoostIc.parameters = {{"Function", "DC-DC converter"}, {"Topology", "Buck-Boost"}, {"Package / Case", "QFN-24"}};
      expectHeader(buckBoostIc, "Buck-Boost Conv.");
    }

    {
      InventoryItem hBridge;
      hBridge.id = "hbridge-1";
      hBridge.partName = "H-bridge motor driver";
      hBridge.manufacturer = "STMicroelectronics";
      hBridge.category = "Integrated Circuits";
      hBridge.parameters = {{"Function", "Motor control"}, {"Output Type", "H-Bridge"}};
      expectHeader(hBridge, "H-Bridge");
    }

    {
      InventoryItem motorDriver;
      motorDriver.id = "motor-driver-1";
      motorDriver.partName = "Dual motor driver";
      motorDriver.manufacturer = "Texas Instruments";
      motorDriver.category = "Integrated Circuits";
      motorDriver.parameters = {{"Function", "Motor Driver"}, {"Output Type", "Half Bridge"}};
      expectHeader(motorDriver, "Motor Driver");
    }

    {
      InventoryItem voltageRef;
      voltageRef.id = "ref-1";
      voltageRef.partName = "Precision voltage reference";
      voltageRef.manufacturer = "Microchip";
      voltageRef.category = "Integrated Circuits";
      voltageRef.parameters = {{"Voltage Reference Type", "Shunt"}, {"Package / Case", "SOT-23"}};
      expectHeader(voltageRef, "Voltage Ref.");
    }

    {
      InventoryItem comparator;
      comparator.id = "cmp-1";
      comparator.partName = "Dual comparator";
      comparator.manufacturer = "onsemi";
      comparator.category = "Integrated Circuits";
      comparator.parameters = {{"Function", "Comparator"}, {"Package / Case", "SOIC-8"}};
      expectHeader(comparator, "Comparator");
    }

    {
      InventoryItem logicBuffer;
      logicBuffer.id = "logic-buffer-1";
      logicBuffer.partName = "Tri-state buffer";
      logicBuffer.manufacturer = "Nexperia";
      logicBuffer.category = "Integrated Circuits";
      logicBuffer.parameters = {{"Function", "Logic Buffer"}, {"Package / Case", "TSSOP-14"}};
      expectHeader(logicBuffer, "Logic Buffer");
    }

    {
      InventoryItem logicGate;
      logicGate.id = "logic-gate-1";
      logicGate.partName = "Quad NAND gate";
      logicGate.manufacturer = "Texas Instruments";
      logicGate.category = "Integrated Circuits";
      logicGate.parameters = {{"Function", "Logic Gate"}, {"Package / Case", "SOIC-14"}};
      expectHeader(logicGate, "Logic Gate");
    }

    {
      InventoryItem tempSensor;
      tempSensor.id = "temp-sensor-1";
      tempSensor.partName = "Digital temperature sensor";
      tempSensor.manufacturer = "Texas Instruments";
      tempSensor.category = "Sensors";
      tempSensor.parameters = {{"Sensor Type", "Temperature"}, {"Output", "I2C"}, {"Resolution", "12 bit"}};
      expectHeader(tempSensor, "Temp Sensor");
    }

    {
      InventoryItem ne555;
      ne555.id = "ne555-timer-1";
      ne555.partName = "NE555 precision timer";
      ne555.manufacturer = "Texas Instruments";
      ne555.category = "Clock/Timing - Programmable Timers and Oscillators";
      ne555.notes = "Operating temperature -40C to 85C.";
      ne555.parameters = {{"Type", "555 Type, Timer/Oscillator (Single)"},
                          {"Frequency", "100kHz"},
                          {"Package / Case", "8-DIP"}};
      expectHeader(ne555, "Timer IC");
      assert(partShortDescription(ne555) == "Timer IC");
    }

    {
      InventoryItem precisionTimer;
      precisionTimer.id = "generic-precision-timer-1";
      precisionTimer.partName = "Precision timer";
      precisionTimer.manufacturer = "Timer Devices Inc.";
      precisionTimer.category = "Clock/Timing - Programmable Timers and Oscillators";
      precisionTimer.parameters = {{"Type", "555 Type, Timer/Oscillator (Single)"},
                                   {"Frequency", "100kHz"},
                                   {"supplier Programmable", "Not Verified"},
                                   {"Package / Case", "8-DIP"}};
      expectHeader(precisionTimer, "Timer IC");
    }

    {
      InventoryItem hostileTimer;
      hostileTimer.id = "timer-hostile-memory-category";
      hostileTimer.partName = "Analog timing controller";
      hostileTimer.manufacturer = "Timer Devices Inc.";
      hostileTimer.category = "Memory";
      hostileTimer.notes = "Timer/Oscillator IC imported with an incorrect broad category.";
      hostileTimer.parameters = {{"Type", "Programmable Timer"}, {"Package / Case", "SOIC-8"}};
      const auto hostileTimerPlan = expectHeader(hostileTimer, "Timer IC");
      assert(hostileTimerPlan.categoryHeader != "Memory IC");
    }

    {
      InventoryItem oneShotTimer;
      oneShotTimer.id = "one-shot-timer-1";
      oneShotTimer.partName = "Monostable multivibrator";
      oneShotTimer.manufacturer = "Timer Devices Inc.";
      oneShotTimer.category = "Integrated Circuits";
      oneShotTimer.parameters = {{"Function", "One-Shot Timer"}, {"Package / Case", "SOIC-14"}};
      expectHeader(oneShotTimer, "Timer IC");
    }

    InventoryItem opAmp;
    opAmp.id = "opamp-1";
    opAmp.partName = "Dual op amp";
    opAmp.manufacturer = "Texas Instruments";
    opAmp.category = "Integrated Circuits";
    opAmp.parameters = {{"Gain Bandwidth", "10MHz"}, {"Slew Rate", "5V/us"}};
    const auto opAmpPlan = expectHeader(opAmp, "OP-AMP");
    assert(opAmpPlan.categoryHeader == "OP-AMP");
    assert(service.buildZpl(opAmp).find("^FDOP-AMP^FS") != string::npos);

    InventoryItem imu;
    imu.id = "imu-1";
    imu.partName = "3-axis IMU";
    imu.manufacturer = "TDK InvenSense";
    imu.category = "Sensors";
    imu.parameters = {{"Sensor Type", "3-axis accelerometer / gyroscope"},
                      {"Output", "I2C"},
                      {"Voltage - Supply", "1.8V"},
                      {"Resolution", "16bit"}};
    const auto imuPlan = expectHeader(imu, "3 Axis IMU");
    assert(imuPlan.categoryHeader == "3 Axis IMU");
    assert(imuPlan.parameterLine1.find("Type") != string::npos);
    assert(imuPlan.parameterLine2.find("Out") != string::npos);
    assert(imuPlan.parameterLine3.find("Vdd") != string::npos || imuPlan.parameterLine3.find("Res") != string::npos);
    assert(service.buildZpl(imu).find("^FD3 Axis IMU^FS") != string::npos);

    {
      InventoryItem buckFalsePositive;
      buckFalsePositive.id = "buck-false-positive-1";
      buckFalsePositive.partName = "Buck converter";
      buckFalsePositive.manufacturer = "Texas Instruments";
      buckFalsePositive.category = "Memory";
      buckFalsePositive.notes = "Integrated circuit with diode clamp, memory-mode setup bits, and rectifier-style flyback protection.";
      buckFalsePositive.parameters = {{"Function", "DC-DC converter"}, {"Topology", "Buck"}, {"Package / Case", "QFN-16"}};
      const auto falsePositivePlan = expectHeader(buckFalsePositive, "Buck Converter");
      assert(service.buildZpl(buckFalsePositive).find("Rectifier Diode") == string::npos);
      assert(service.buildZpl(buckFalsePositive).find("Memory IC") == string::npos);
      assert(service.buildZpl(buckFalsePositive).find("^FDBuck Converter^FS") != string::npos);
      assert(falsePositivePlan.packageLine.find("QFN-16") != string::npos);
    }

    {
      InventoryItem genericIc;
      genericIc.id = "generic-ic-from-text";
      genericIc.partName = "Configurable analog controller";
      genericIc.manufacturer = "Acme";
      genericIc.category = "Integrated Circuits";
      genericIc.notes = "Operates from a single supply.";
      genericIc.parameters = {{"Function", "Controller"}, {"Package / Case", "SOIC-8"}};
      const auto genericPlan = expectHeader(genericIc, "Integrated Circuit");
      assert(genericPlan.categoryHeader == genericIc.cataloguePrintLabel);
      assert(genericPlan.categoryHeader != "Memory IC");
    }

    InventoryItem fallback;
    fallback.id = "misc-1";
    fallback.partName = "Prototype module";
    fallback.manufacturer = "Acme";
    fallback.category = "Misc / Prototype";
    const auto fallbackPlan = expectHeader(fallback, "Misc");
    assert(fallbackPlan.categoryHeader == "Misc");
    assert(service.buildZpl(fallback).find("^FDMisc^FS") != string::npos);

    InventoryItem inductor;
    inductor.id = "ind-1";
    inductor.partName = "RF inductor";
    inductor.manufacturer = "Murata";
    inductor.category = "Inductors";
    inductor.notes = "FIXED IND 27NH 350MA 460 MOHM";
    inductor.parameters = {{"Value", "100MHz"}, {"Current Rating", "350mA"}, {"Frequency - Self Resonant", "1.7GHz"}};
    const auto inductorFields = electricalFieldsForItem(inductor);
    bool foundInductance = false;
    for (const auto& field : inductorFields) {
      assert(!(field.label.find("Inductance") != string::npos && field.value == "100MHz"));
      if (field.label.find("Inductance") != string::npos) {
        foundInductance = true;
        assert(field.value == "27nH");
      }
    }
    assert(foundInductance);
    const auto inductorPlan = service.buildLabelPlan(inductor);
    assert(inductorPlan.mainValue.find("100MHz") == string::npos);
    assert(inductorPlan.mainValue.find("27nH") != string::npos);
  }

  {
    auto backend = make_unique<MockPrinterBackend>();
    LabelPrinterService service(move(backend));
    InventoryItem item;
    item.id = "mcu-1";
    item.partName = "STM32G0 demo board";
    item.manufacturer = "STMicroelectronics";
    item.category = "MCUs";
    item.inventatoryId = "Inventatory:M-00045";
    item.quantity = 12;
    item.lastUpdated = 1710000000;
    item.createdAt = 1710000000;
    item.parameters = {
        {"Package / Case", "LQFP-64"},
        {"Operating Voltage", "3.3V"},
        {"Core", "Cortex-M0+"},
        {"Clock Speed", "64MHz"},
        {"Flash", "128KB"},
        {"RAM", "36KB"},
    };

    const auto plan = service.buildLabelPlan(item);
    assert(plan.mainValue == "STM32G0 demo board");
    assert(plan.packageLine.find("LQFP-64") != string::npos);
    assert(plan.manufacturerLine == "STMicroelectronics");
    assert(plan.parameterLine1.find("Vdd") != string::npos);
    assert(plan.parameterLine1.find("3.3V") != string::npos);
    assert(plan.parameterLine2.find("Core") != string::npos);
    assert(plan.parameterLine2.find("Cortex-M0+") != string::npos);
    assert(plan.parameterLine2.find("@") != string::npos);
    assert(plan.parameterLine3.find("Flash") != string::npos);
    assert(plan.parameterLine3.find("128KB") != string::npos);
  }

  {
    auto backend = make_unique<MockPrinterBackend>();
    LabelPrinterService service(move(backend));
    InventoryItem item;
    item.id = "mosfet-1";
    item.partName = "N-channel MOSFET";
    item.manufacturer = "Alpha & Omega";
    item.category = "MOSFETs";
    item.inventatoryId = "Inventatory:T-00012";
    item.parameters = {
        {"Package / Case", "TO-263-3, D2PAK (2 Leads + Tab)"},
        {"Drain-Source Voltage", "30V"},
        {"Continuous Drain Current", "12A"},
        {"Power - Max", "2W (Ta)"},
    };

    const auto plan = service.buildLabelPlan(item);
    assert(plan.packageLine == "TO-263-3");
    assert(plan.parameterLine1.find("Vds") != string::npos);
    assert(plan.parameterLine1.find("30V") != string::npos);
    assert(plan.parameterLine2.find("Id") != string::npos);
    assert(plan.parameterLine2.find("12A") != string::npos);
    assert(plan.parameterLine3.empty());

    const auto zpl = service.buildZpl(item);
    assert(zpl.find("2W (Ta)") == string::npos);
    assert(zpl.find("TO-263-3,") == string::npos);
    assert(zpl.find("^FO10,70^A0N,14,14^FDTO-263-3^FS") != string::npos);
    assert(zpl.find("^FDVds 30V^FS") != string::npos);
    assert(zpl.find("^FDId 12A^FS") != string::npos);
  }

  {
    auto backend = make_unique<MockPrinterBackend>();
    LabelPrinterService service(move(backend));

    InventoryItem diode;
    diode.id = "diode-1";
    diode.partName = "SS14";
    diode.manufacturer = "Diodes Inc.";
    diode.category = "Schottky Diodes";
    diode.parameters = {{"Forward Voltage", "0.38V"}, {"Reverse Voltage", "40V"}, {"Current", "1A"}};
    const auto diodePlan = service.buildLabelPlan(diode);
    assert(diodePlan.parameterLine1.find("Vf") != string::npos);
    assert(diodePlan.parameterLine2.find("Vr") != string::npos);
    assert(diodePlan.parameterLine3.find("Io") != string::npos);

    InventoryItem connector;
    connector.id = "conn-1";
    connector.partName = "Board header";
    connector.manufacturer = "Harwin";
    connector.category = "Connectors";
    connector.parameters = {{"Pins", "8"}, {"Connector Type", "Header"}, {"Pitch", "2.54mm"}, {"Rows", "2"}};
    const auto connectorPlan = service.buildLabelPlan(connector);
    assert(connectorPlan.parameterLine1.find("Pins") != string::npos);
    assert(connectorPlan.parameterLine2.find("Conn") != string::npos);
    assert(connectorPlan.parameterLine3.find("Rows") != string::npos);

    InventoryItem regulator;
    regulator.id = "reg-1";
    regulator.partName = "3.3V LDO";
    regulator.manufacturer = "Microchip";
    regulator.category = "Voltage Regulators";
    regulator.parameters = {{"Output Voltage", "3.3V"}, {"Voltage - Input", "5V"}, {"Output Current", "1A"}, {"Type", "LDO"}};
    const auto regulatorPlan = service.buildLabelPlan(regulator);
    assert(regulatorPlan.parameterLine1.find("Vout") != string::npos);
    assert(regulatorPlan.parameterLine2.find("Vin") != string::npos);
    assert(regulatorPlan.parameterLine3.find("Iout") != string::npos);

    InventoryItem crystal;
    crystal.id = "xtal-1";
    crystal.partName = "16MHz crystal";
    crystal.manufacturer = "Abracon";
    crystal.category = "Crystals";
    crystal.parameters = {{"Frequency", "16MHz"}, {"Load Capacitance", "18pF"}, {"ESR", "50Ohm"}};
    const auto crystalPlan = service.buildLabelPlan(crystal);
    assert(crystalPlan.parameterLine1.find("F") != string::npos);
    assert(crystalPlan.parameterLine2.find("ESR") != string::npos ||
           crystalPlan.parameterLine3.find("ESR") != string::npos);
  }

  {
    const string csv = "Catalog Number,Manufacturer,Description,Quantity\n"
                       "CAT-123,Acme,Missing manufacturer part,3\n";
    const auto result = parseBomCsvText(csv, {});
    assert(!result.ok);
  }

  {
    const string csv = "Manufacturer Part Number,Manufacturer,Description,Quantity\n"
                       "ABC-123,Acme,Overflow quantity,2147483648\n";
    const auto result = parseBomCsvText(csv, {});
    assert(!result.ok);
  }

  {
    DeviceQuantityRequest request;
    string error;
    assert(parseQuantityRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-1","code":"0002","delta":-12})", request, error));
    assert(request.delta == -12);
    assert(!parseQuantityRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-2","code":"0002","delta":0})", request, error));

    InventoryStore store;
    InventoryItem item;
    item.id = "scan-r1-item";
    item.inventatoryId = "Inventatory:R-00123";
    item.machineCode = "0002";
    item.partName = "10k resistor";
    item.quantity = 5;
    store.items().push_back(item);
    request = {"r1-a", "req-3", "0002", -12};
    const auto clamped = applyDeviceQuantity(store, request);
    assert(clamped.ok);
    assert(clamped.requestedDelta == -12);
    assert(clamped.appliedDelta == -5);
    assert(clamped.quantity == 0);
    request = {"r1-a", "req-4", "Inventatory:R-0002", 2};
    assert(applyDeviceQuantity(store, request).httpStatus == 400);
    request = {"r1-a", "req-5", "R123", 2};
    assert(applyDeviceQuantity(store, request).httpStatus == 400);
    request = {"r1-a", "req-6", "308-1571-1-ND", 2};
    assert(applyDeviceQuantity(store, request).httpStatus == 400);
  }

  {
    DeviceScanRequest request;
    string error;
    assert(parseScanRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-1","code":"[)>\u001e06\u001dPRC0603FR-0710KL\u001dQ2\u001e\u0004","quantity":2})",
        request, error));
    assert(request.quantity == 2);
    assert(request.code == string("[)>") + '\x1e' + "06" + '\x1d' + "PRC0603FR-0710KL" + '\x1d' + "Q2" + '\x1e' + '\x04');

    assert(parseScanRequestJson(R"({"deviceId":"r1-a","requestId":"req-2","code":"ABC123"})", request, error));
    assert(request.quantity == 1);
    assert(parseScanRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-3","code":"ABC123","metadata":{"code":"ignored"}})",
        request, error));
    assert(request.code == "ABC123");
    assert(!parseScanRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-4","code":"ABC123","code":"ambiguous"})", request, error));
    assert(!parseScanRequestJson(
        R"({"deviceId":"r1-a","requestId":"req-5","code":"ABC123")", request, error));
  }

  {
    const auto configPath = filesystem::temp_directory_path() / "inventatory-scan-config-test.conf";
    const InventatoryScanConfig expected{"r1-test", string(64, 'a'), "192.168.1.2", 8080};
    assert(saveInventatoryScanConfig(configPath, expected));
    InventatoryScanConfig loaded;
    assert(loadInventatoryScanConfig(configPath, loaded));
    assert(loaded.deviceId == expected.deviceId);
    assert(loaded.token.empty());
    assert(loaded.fallbackHost == expected.fallbackHost);
    assert(loaded.fallbackPort == expected.fallbackPort);
    filesystem::remove(configPath);
    assert(generateInventatoryScanToken().size() == 64);
  }

  {
    InventoryStore store;
    InventoryItem item;
    item.id = "scan-r1-item";
    item.inventatoryId = "Inventatory:R-00123";
    item.machineCode = "0002";
    item.partName = "10k resistor";
    item.quantity = 5;
    store.items().push_back(item);

    DeviceQuantityRequest request{"r1-a", "req-9", "0002", -2};
    unordered_map<string, DeviceQuantityResult> cache;
    deque<string> order;
    const auto first = applyDeviceQuantityCached(store, request, cache, order);
    const auto second = applyDeviceQuantityCached(store, request, cache, order);
    assert(first.ok);
    assert(second.ok);
    assert(first.appliedDelta == -2);
    assert(second.appliedDelta == -2);
    assert(store.items().front().quantity == 3);
    assert(statusResultJson(false, "Unauthorized device").find("Unauthorized device") != string::npos);
  }

  {
    DeviceSyncRequest request;
    string error;
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-1","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":1,"capabilities":["lcd.128x64"],"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":["r1-a-76-result"]})",
        request, error));
    assert(request.protocolVersion == 2);
    assert(request.events.size() == 1);
    assert(request.events.front().value == 2);
    assert(request.resultAcks.size() == 1);
    assert(!request.hasLookup);
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-component","deviceId":"r1-a","firmwareVersion":"0.6.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":"r1-a-78","type":"inventory.receive","code":"TPS7A2033PDBVR","value":5,"component":{"manufacturer":"Texas Instruments","manufacturerPartNumber":"TPS7A2033PDBVR","encodedPartName":"TPS7A20 LDO"}}],"resultAcks":[]})",
        request, error));
    assert(request.events.size() == 1);
    assert(request.events.front().manufacturer == "Texas Instruments");
    assert(request.events.front().manufacturerPartNumber == "TPS7A2033PDBVR");
    assert(request.events.front().encodedPartName == "TPS7A20 LDO");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-name-only","deviceId":"r1-a","firmwareVersion":"0.6.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":"r1-a-79","type":"inventory.receive","code":"","value":1,"component":{"manufacturer":"","manufacturerPartNumber":"","encodedPartName":"Unidentified sensor"}}],"resultAcks":[]})",
        request, error));
    assert(request.events.front().manufacturerPartNumber.empty());
    assert(request.events.front().encodedPartName == "Unidentified sensor");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-lookup","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-9","code":"0002"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.lookupId == "lookup-9");
    assert(request.lookup.code == "0002");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-mpn-lookup","deviceId":"r1-a","firmwareVersion":"0.5.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-mpn-1","code":"RC0603FR-0710KL"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.code == "RC0603FR-0710KL");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-special-mpn","deviceId":"r1-a","firmwareVersion":"0.5.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-mpn-2","code":"DS3231S#T&R"}})",
        request, error));
    assert(request.lookup.code == "DS3231S#T&R");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-label","deviceId":"r1-a","firmwareVersion":"0.4.0","mode":"label_print","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"quickLabelPrint":{"requestId":"r1-a-label-1","presetIndex":2,"revision":3}})",
        request, error));
    assert(request.hasQuickLabelPrint);
    assert(request.quickLabelPrint.presetIndex == 2);
    assert(request.quickLabelPrint.revision == 3);
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-bad-lookup","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-10","code":"bad:code"}})",
        request, error));

    DeviceSyncResponse quickLabelResponse;
    quickLabelResponse.requestId = "sync-label";
    quickLabelResponse.hasQuickLabels = true;
    quickLabelResponse.quickLabelRevision = 3;
    quickLabelResponse.quickLabelPresets = {"5V", "12V"};
    quickLabelResponse.hasQuickLabelPrintResult = true;
    quickLabelResponse.quickLabelPrintResult = {"r1-a-label-1", "completed", "", "Label sent"};
    const auto quickLabelJson = deviceSyncResponseJson(quickLabelResponse);
    assert(quickLabelJson.find("\"quickLabels\":{\"revision\":3") != string::npos);
    assert(quickLabelJson.find("\"quickLabelPrintResult\":{\"requestId\":\"r1-a-label-1\"") != string::npos);
    assert(quickLabelJson.find("catalogue") == string::npos);
    assert(quickLabelJson.find("properties") == string::npos);
    assert(quickLabelJson.find("rawValue") == string::npos);
    quickLabelResponse.lookupResult = {"lookup-control", "found", string("part") + '\x01'};
    quickLabelResponse.hasLookupResult = true;
    assert(deviceSyncResponseJson(quickLabelResponse).find("part\\u0001") != string::npos);
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-2","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[]})",
        request, error));

    const auto databasePath = filesystem::temp_directory_path() / "inventatory-device-sync-v1-test.db";
    filesystem::remove(databasePath);
    InventoryStore store;
    InventoryItem item;
    item.id = "sync-item";
    item.machineCode = "0002";
    item.partName = "10k resistor";
    item.manufacturerPartNumber = "RC0603FR-0710KL";
    item.quantity = 5;
    item.location = "R1-A1";
    store.items().push_back(item);
    assert(store.save(databasePath));

    const auto beforeLookupQuantity = store.items().front().quantity;
    InventoryStore lookupSnapshot;
    assert(lookupSnapshot.load(databasePath));
    const auto foundLookup = lookupDeviceItem(lookupSnapshot, {"lookup-9", "0002"});
    assert(foundLookup.status == "found");
    assert(foundLookup.itemName == "10k resistor");
    assert(lookupSnapshot.items().front().quantity == beforeLookupQuantity);
    const auto databaseFoundLookup = lookupDeviceItem(databasePath, {"lookup-9-db", "0002"});
    assert(databaseFoundLookup.status == "found");
    assert(databaseFoundLookup.itemName == "10k resistor");
    const auto mpnLookup = lookupDeviceItem(lookupSnapshot, {"lookup-mpn-1", "RC0603FR-0710KL"});
    assert(mpnLookup.status == "found");
    assert(mpnLookup.itemName == "10k resistor");
    const auto databaseMpnLookup =
        lookupDeviceItem(databasePath, {"lookup-mpn-1-db", "RC0603FR-0710KL"});
    assert(databaseMpnLookup.status == "found");
    assert(databaseMpnLookup.itemName == "10k resistor");
    const auto missingLookup = lookupDeviceItem(lookupSnapshot, {"lookup-10", "9999"});
    assert(missingLookup.status == "not_found");
    const auto databaseMissingLookup = lookupDeviceItem(databasePath, {"lookup-10-db", "9999"});
    assert(databaseMissingLookup.status == "not_found");

    DeviceSyncResponse lookupResponse;
    lookupResponse.requestId = "sync-lookup";
    lookupResponse.hasLookupResult = true;
    lookupResponse.lookupResult = foundLookup;
    const auto lookupJson = deviceSyncResponseJson(lookupResponse);
    assert(lookupJson.find("\"lookupId\":\"lookup-9\"") != string::npos);
    assert(lookupJson.find("\"status\":\"found\"") != string::npos);
    assert(lookupJson.find("10k resistor") != string::npos);
    lookupResponse.lookupResult = {"lookup-10", "unavailable", {}};
    const auto unavailableJson = deviceSyncResponseJson(lookupResponse);
    assert(unavailableJson.find("\"status\":\"unavailable\"") != string::npos);
    lookupResponse.lookupResult = missingLookup;
    const auto missingJson = deviceSyncResponseJson(lookupResponse);
    assert(missingJson.find("\"status\":\"not_found\"") != string::npos);

    request = {};
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-3","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":[]})",
        request, error));
    DeviceSyncResponse response;
    assert(acceptDeviceSyncEvents(databasePath, request, response, error));
    assert(response.acceptedEventIds.size() == 1);
    assert(response.results.empty());
    const auto pending = loadPendingDeviceSyncEvents(databasePath);
    assert(pending.size() == 1);

    auto candidate = store;
    DeviceQuantityRequest quantityRequest{"r1-a", pending.front().eventId, pending.front().code,
                                          pending.front().value};
    const auto quantity = applyDeviceQuantity(candidate, quantityRequest);
    assert(quantity.ok);
    DeviceSyncResult result;
    result.resultId = pending.front().eventId + "-result";
    result.eventId = pending.front().eventId;
    result.status = "completed";
    result.existing = true;
    result.itemName = quantity.item;
    result.requestedDelta = pending.front().value;
    result.appliedDelta = quantity.appliedDelta;
    result.quantity = quantity.quantity;
    result.location = "R1-A1";
    result.message = "Quantity updated";

    // A new protocol-v2 receive is auto-printed immediately after its event is
    // committed. Its live item must receive the same identifiers as the SQLite
    // snapshot; otherwise the label has blank Inventatory text and an empty QR field.
    InventoryItem autoLabelItem;
    autoLabelItem.id = "auto-label-new-item";
    autoLabelItem.partName = "Auto label IC";
    autoLabelItem.category = "Integrated Circuits";
    autoLabelItem.lastUpdated = time(nullptr);
    autoLabelItem.createdAt = autoLabelItem.lastUpdated;
    candidate.items().push_back(autoLabelItem);
    assert(completeDeviceSyncEvent(candidate, databasePath, result));
    const auto* finalizedAutoLabelItem = candidate.findById(autoLabelItem.id);
    assert(finalizedAutoLabelItem != nullptr);
    assert(isInventatoryId(finalizedAutoLabelItem->inventatoryId));
    assert(finalizedAutoLabelItem->machineCode.size() == 4);
    const auto autoLabelPlan = LabelPrinterService{}.buildLabelPlan(*finalizedAutoLabelItem);
    assert(autoLabelPlan.scannerHint == buildVisibleInventatoryId(*finalizedAutoLabelItem));
    assert(autoLabelPlan.barcodeHint == finalizedAutoLabelItem->machineCode);

    response = {};
    assert(acceptDeviceSyncEvents(databasePath, request, response, error));
    assert(response.acceptedEventIds.size() == 1);
    assert(response.results.size() == 1);
    assert(response.results.front().quantity == 7);
    InventoryStore reloaded;
    assert(reloaded.load(databasePath));
    assert(reloaded.findByMachineCode("0002")->quantity == 7);

    request.events.clear();
    request.resultAcks = {result.resultId};
    request.requestId = "sync-4";
    response = {};
    assert(acceptDeviceSyncEvents(databasePath, request, response, error));
    assert(response.results.empty());
    filesystem::remove(databasePath);
  }

  {
    const ftxui::Box box{2, 6, 4, 8};
    assert(uiBoxContains(box, 2, 4));
    assert(uiBoxContains(box, 6, 8));
    assert(uiBoxContains(box, 4, 6));
    assert(!uiBoxContains(box, 1, 6));
    assert(!uiBoxContains(box, 4, 9));
  }

  {
    const auto xlsxPath = filesystem::temp_directory_path() / "ti-products-synthetic.xlsx";
    error_code ignored;
    filesystem::remove(xlsxPath, ignored);
    assert(writeSyntheticXlsx(xlsxPath));
    CatalogueTable table;
    string xlsxError;
    assert(readXlsxCatalogueTable(xlsxPath, "products", table, xlsxError));
    assert(table.sheetName == "Products");
    assert(table.headers.size() == 7);
    assert(table.rows.size() == 2);
    assert(table.rows[0][0] == "OPA333AIDBVR");
    assert(table.rows[0][2] == "0402");
    assert(table.rows[0][3] == "2");
    assert(table.rows[1][3].empty());
    atomic_bool cancelled = true;
    assert(!readXlsxCatalogueTable(xlsxPath, "products", table, xlsxError, &cancelled));
    assert(xlsxError == "Workbook import cancelled");
    assert(!readXlsxCatalogueTable(xlsxPath.string() + ".missing", "products", table, xlsxError));
    filesystem::remove(xlsxPath, ignored);
  }

  {
    assert(manufacturerSources().size() == 8);
    for (const auto& source : manufacturerSources()) {
      assert(source.downloadMode == DownloadMode::BrowserGuided);
      assert(!source.automaticFetchAllowed);
      assert(!source.redistributionAllowed);
    }
    auto microfarads = parseEngineeringValue("4.7 µF");
    assert(microfarads.nominal.has_value());
    assert(abs(*microfarads.nominal - 4.7e-6) < 1e-12);
    assert(microfarads.unit == "F");
    auto contextual = parseEngineeringValue("3 m", "Ohm");
    assert(contextual.nominal.has_value() && abs(*contextual.nominal - 0.003) < 1e-12);
    auto range = parseEngineeringValue("-55 to +125 °C");
    assert(range.minimum == -55 && range.maximum == 125 && range.unit == "degC");
    assert(parseEngineeringValue("10k").warning.size() > 0);
    assert(detectCatalogueProfile("unrecognised-export.csv", {"Part Number", "Description"}) == nullptr);
    assert(detectCatalogueProfile("TI_opamps_synthetic.csv", {"Orderable Part Number", "Channels", "Supply voltage (min)"}) != nullptr);
    const auto profileHasProperty = [](const CatalogueProfile& profile, const string& name) {
      return any_of(profile.properties.begin(), profile.properties.end(), [&](const PropertyMapping& property) { return property.canonicalName == name; });
    };
    const auto tiProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "ti-parametric-v1"; });
    const auto adiProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "adi-amplifiers-v1"; });
    const auto microchipProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "microchip-parametric-v1"; });
    const auto murataProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "murata-capacitors-v1"; });
    const auto tdkProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "tdk-mlcc-v1"; });
    const auto nexperiaProfile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& profile) { return profile.id == "nexperia-discretes-v1"; });
    assert(tiProfile != catalogueProfiles().end() && profileHasProperty(*tiProfile, "quiescent_current") && profileHasProperty(*tiProfile, "gate_charge"));
    assert(adiProfile != catalogueProfiles().end() && profileHasProperty(*adiProfile, "noise_density") && profileHasProperty(*adiProfile, "slew_rate"));
    assert(microchipProfile != catalogueProfiles().end() && profileHasProperty(*microchipProfile, "dac_resolution") && profileHasProperty(*microchipProfile, "usb_interfaces"));
    assert(murataProfile != catalogueProfiles().end() && profileHasProperty(*murataProfile, "dimensions") && profileHasProperty(*murataProfile, "operating_temperature"));
    assert(tdkProfile != catalogueProfiles().end() && profileHasProperty(*tdkProfile, "packaging") && profileHasProperty(*tdkProfile, "insulation_resistance"));
    assert(nexperiaProfile != catalogueProfiles().end() && profileHasProperty(*nexperiaProfile, "reverse_recovery_time") && profileHasProperty(*nexperiaProfile, "dc_current_gain"));

    const auto databasePath = filesystem::temp_directory_path() / "inventatory-catalogues-test.db";
    const auto csvPath = filesystem::temp_directory_path() / "TI_opamps_synthetic.csv";
    const auto unknownPath = filesystem::temp_directory_path() / "unknown-supplier-table.csv";
    error_code ignored;
    filesystem::remove(databasePath, ignored);
    ofstream csv(csvPath, ios::binary | ios::trunc);
    csv << "\xEF\xBB\xBFOrderable Part Number,Generic Part Number,Package,Channels,Supply voltage (min),Supply voltage (max),Description\n"
           "OPA333AIDBVR,OPA333,SOT-23,1,1.8 V,5.5 V,Zero-drift amplifier\n"
           "\"OPA,QUOTED\",BASE2,0402,2,2.7 V,12 V,\"quoted, description\"\n"
           "BROKEN\"ROW,BASE3,SOT-23,1,1 V,2 V,Bad row\n";
    csv.close();
    CatalogueDatabase database;
    assert(database.open(databasePath));
    {
      ofstream unknown(unknownPath, ios::binary | ios::trunc);
      unknown << "Supplier code,Case,Selection flag\nABC-001,0402,Preferred\n";
    }
    const auto unknownPreview = database.previewFile(unknownPath);
    assert(unknownPreview.valid());
    assert(unknownPreview.profileId.empty());
    assert(unknownPreview.headers.size() == 3);
    assert(!unknownPreview.warnings.empty());
    CatalogueProfile manualProfile{"ti-parametric-v1", "manual-v1", "Texas Instruments", "Custom", "", {},
                                   {"Supplier code"}, {}, {}, {"Case"}, {}, {}, {}, {}, false};
    manualProfile.properties.push_back({{"Selection flag"}, "automotive_qualification", "", ""});
    assert(database.saveLocalMapping(manualProfile));
    const auto savedManualProfile = database.localMapping("ti-parametric-v1");
    assert(savedManualProfile.has_value());
    assert(savedManualProfile->mpnColumns == vector<string>{"Supplier code"});
    assert(savedManualProfile->packageColumns == vector<string>{"Case"});
    assert(savedManualProfile->properties.size() == 1);
    assert(savedManualProfile->properties.front().canonicalName == "automotive_qualification");
    const auto manuallyImported = database.importFile(unknownPath, &manualProfile);
    assert(manuallyImported.error.empty());
    const auto manualMatch = database.lookup("Texas Instruments", "ABC-001");
    assert(manualMatch.matched());
    assert(any_of(manualMatch.record.properties.begin(), manualMatch.record.properties.end(), [](const CatalogueProperty& property) {
      return property.sourceColumn == "Selection flag" && property.name == "automotive_qualification" && property.mappingStatus == "warning";
    }));
    const auto imported = database.importFile(csvPath);
    assert(imported.error.empty());
    assert(imported.parts == 2);
    assert(imported.rejected == 1);
    assert(imported.properties >= 6);
    const auto snapshots = database.snapshots();
    assert(!snapshots.empty());
    const auto importedSnapshot = find_if(snapshots.begin(), snapshots.end(), [](const CatalogueImportStats& snapshot) {
      return snapshot.filename == "TI_opamps_synthetic.csv";
    });
    assert(importedSnapshot != snapshots.end());
    assert(importedSnapshot->profileVersion == "1");
    assert(database.importFile(csvPath).duplicate);
    const auto exact = database.lookup("Texas Instruments", " opa333aidbvr ");
    assert(exact.status == CatalogueMatchStatus::ExactMatch);
    assert(exact.record.properties.size() >= 3);
    const auto alias = database.lookup("Texas Instruments", "OPA333");
    assert(alias.status == CatalogueMatchStatus::AliasMatch);
    assert(database.lookup("Texas Instruments", "OPA-333").status == CatalogueMatchStatus::NotFound);
    InventoryItem item;
    item.partName = "Manual name";
    item.manufacturerPartNumber = "OPA333AIDBVR";
    item.parameters = {{"Manual property", "keep me"}};
    assert(applyCatalogueEnrichment(item, exact));
    assert(item.partName == "Manual name");
    assert(item.parameters.size() == 1 && item.parameters.front().name == "Manual property" && item.parameters.front().value == "keep me");
    assert(serializeItem(item).find("Shutdown current") == string::npos);
    assert(item.catalogueStatus == "exact_match");
    assert(item.catalogueMatched());
    assert(!item.hasMissingMetadata());
    item.catalogueStatus = "alias_match";
    assert(item.catalogueMatched());
    item.catalogueStatus = "not_in_catalogue";
    assert(!item.catalogueMatched());
    item.catalogueStatus = "exact_match";
    const auto xlsxPath = filesystem::temp_directory_path() / "ti-products-synthetic.xlsx";
    assert(writeSyntheticXlsx(xlsxPath));
    const auto profile = find_if(catalogueProfiles().begin(), catalogueProfiles().end(), [](const CatalogueProfile& candidate) {
      return candidate.id == "ti-parametric-v1";
    });
    assert(profile != catalogueProfiles().end());
    const auto preview = database.previewFile(xlsxPath, &*profile);
    assert(preview.valid());
    assert(preview.sheetName == "Products");
    assert(preview.rows == 2);
    assert(find(preview.unmappedColumns.begin(), preview.unmappedColumns.end(), "Shutdown current") != preview.unmappedColumns.end());
    const auto xlsxImported = database.importFile(xlsxPath, &*profile);
    assert(xlsxImported.error.empty());
    assert(xlsxImported.parts == 2);
    assert(xlsxImported.properties >= 7);
    const auto xlsxExact = database.lookup("Texas Instruments", "OPA333AIDBVR");
    assert(any_of(xlsxExact.record.properties.begin(), xlsxExact.record.properties.end(), [](const CatalogueProperty& property) {
      return property.sourceColumn == "Shutdown current" && property.mappingStatus == "unmapped" && property.rawValue == "1 µA";
    }));
    auto newerTiProfile = *profile;
    newerTiProfile.version = "2";
    newerTiProfile.properties.push_back({{"Shutdown current"}, "shutdown_current", "A", ""});
    assert(database.reprocessSource(newerTiProfile) == 2);
    const auto remapped = database.lookup("Texas Instruments", "OPA333AIDBVR");
    assert(any_of(remapped.record.properties.begin(), remapped.record.properties.end(), [](const CatalogueProperty& property) {
      return property.name == "shutdown_current" && property.mappingStatus == "mapped" && property.value.rawValue == "1 µA";
    }));
    assert(database.lookup("Texas Instruments", "OPA333AIDBVR-REEL").matched());
    assert(database.removeSource("ti"));
    const auto removed = database.lookup("Texas Instruments", "OPA333AIDBVR");
    assert(removed.status == CatalogueMatchStatus::NotFound);
    assert(applyCatalogueEnrichment(item, removed));
    assert(item.cataloguePartId.empty());
    assert(item.catalogueName.empty());
    assert(item.catalogueStatus == "not_in_catalogue");
    assert(item.partName == "Manual name");
    filesystem::remove(csvPath, ignored);
    filesystem::remove(unknownPath, ignored);
    filesystem::remove(xlsxPath, ignored);
    filesystem::remove(databasePath, ignored);
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-app-settings-test.conf";
    AppSettings expected;
    expected.dataDirectory = filesystem::temp_directory_path() / "Inventatory test data";
    expected.printerQueue = "ZDesigner Test Queue";
    expected.autoPrintScannedLabels = false;
    expected.backgroundServiceEnabled = true;
    expected.backgroundConsentAsked = true;
    expected.completedOnboardingVersion = 1;
    expected.updateChecksEnabled = false;
    expected.lastUpdateCheckUnixSeconds = 123456789;
    expected.latestAvailableVersion = "0.2.0";
    expected.latestReleaseUrl = "https://github.com/Kwiatens/Inventatory-Software/releases/tag/v0.2.0";
    expected.deviceServicePort = 8181;
    expected.quickLabelPresets = {"5V", "GND", "12V"};
    expected.quickLabelRevision = 9;
    assert(saveAppSettings(path, expected));

    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.schemaVersion == 3);
    assert(loaded.completedOnboardingVersion == 1);
    assert(loaded.dataDirectory == expected.dataDirectory);
    assert(loaded.printerQueue == expected.printerQueue);
    assert(!loaded.autoPrintScannedLabels);
    assert(loaded.backgroundServiceEnabled);
    assert(loaded.backgroundConsentAsked);
    assert(!loaded.updateChecksEnabled);
    assert(loaded.lastUpdateCheckUnixSeconds == 123456789);
    assert(loaded.latestAvailableVersion == "0.2.0");
    assert(loaded.latestReleaseUrl == expected.latestReleaseUrl);
    assert(loaded.deviceServicePort == 8181);
    assert(loaded.quickLabelPresets == expected.quickLabelPresets);
    assert(loaded.quickLabelRevision == 9);

    ifstream persisted(path);
    const string text((istreambuf_iterator<char>(persisted)), istreambuf_iterator<char>());
    assert(text.find("device_service_port=8181") != string::npos);
    assert(text.find("bridge_port") == string::npos);
    persisted.close();
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    assert(buildBackgroundStartupCommand(L"C:\\Program Files\\Inventatory\\inventatory.exe") ==
           L"\"C:\\Program Files\\Inventatory\\inventatory.exe\" --background");
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-legacy-settings-test.conf";
    ofstream legacy(path, ios::trunc);
    legacy << "schema_version=1\n"
           << "bridge_port=8182\n";
    legacy.close();
    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.schemaVersion == 3);
    assert(loaded.deviceServicePort == 8182);
    assert(!loaded.backgroundServiceEnabled);
    assert(!loaded.backgroundConsentAsked);
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    assert(isVersionNewer("v0.2.0", "0.1.9"));
    assert(isVersionNewer("0.2", "0.1.9"));
    assert(!isVersionNewer("0.2.0", "0.2.0"));
    assert(!isVersionNewer("preview", "0.2.0"));
    assert(isUpdateCheckDue(true, 0, 100));
    assert(!isUpdateCheckDue(false, 0, 100));
    assert(!isUpdateCheckDue(true, 100, 100 + 60));
    assert(isUpdateCheckDue(true, 100, 100 + 24 * 60 * 60));
  }

  cout << "Inventatory core tests passed\n";
  return 0;
}
