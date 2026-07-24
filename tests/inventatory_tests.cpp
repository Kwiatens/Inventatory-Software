#include "core/Inventory.h"
#include "app/AppSettings.h"
#include "platform/UpdateService.h"
#include "platform/StartupRegistration.h"
#include "core/InventoryInternals.h"
#include "core/InventatoryScanProtocol.h"
#include "core/PartDescriptor.h"
#include "import/DigiKeyCsvImport.h"
#include "import/CsvFormat.h"
#include "import/CsvReader.h"
#include "import/KicadBom.h"
#include "core/BomMatch.h"
#include "core/BomProjectStore.h"
#include "label_printer/LabelPrinter.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

vector<InventoryItem> makeSampleInventory() {
  vector<InventoryItem> items;

  items.push_back({
      "res-0603-10k",
      "10k Resistor 0603",
      "Yageo",
      "Resistors",
      180,
      50,
      "Shelf A3",
      {"0603", "1%", "rohs"},
      {{"Resistance", "10k Ohm"}, {"Power", "0.1W"}, {"Package", "0603"}},
      "General purpose pull-up and divider resistor.",
      "311-10.0KHRCT-ND",
      "https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729604",
      "https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729604",
      "synced",
      "RC0603FR-0710KL",
      1710000000,
  });

  items.push_back({
      "esp32-s3-module",
      "ESP32-S3 Module",
      "Espressif",
      "MCUs",
      4,
      10,
      "ESD Drawer",
      {"wifi", "bluetooth", "module"},
      {{"Core", "Xtensa LX7"}, {"Flash", "16MB"}, {"Package", "Module"}},
      "Used for integration prototypes and test rigs.",
      "1965-ESP32-S3-MODULE-ND",
      "https://www.digikey.com/en/products/detail/espressif-systems/ESP32-S3/15240400",
      "https://www.digikey.com/en/products/detail/espressif-systems/ESP32-S3/15240400",
      "synced",
      "ESP32-S3-WROOM-1",
      1710000100,
  });

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

int main() {
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

    const auto resolution = resolveScanCode(store, "311-10.0KHRCT-ND");
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

    const auto created = resolveScanCode(store, "new-digikey-code");
    assert(created.matched);
    assert(created.created);
    assert(store.findById(created.itemId) != nullptr);
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
    assert(buildVisibleInventatoryId(items[1]) == "C-0002");
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
    item.digikeyPartNumber = "123";
    item.datasheetUrl = "https://example.com/datasheet";
    item.productUrl = "https://example.com/product";
    item.syncStatus = "synced";
    item.sku = "SKU-1";
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
        "Indeks,Nr kat. DigiKey,Manufacturer Part Number,Producent,Opis,Numer referencyjny klienta,Ilość,"
        "Niezrealizowana pozycja zamówienia,Cena jednostkowa,Wartość\n"
        "1,308-1571-1-ND,CDMC6D28NP-4R7MC,Sumida America Components Inc.,FIXED IND 4.7UH 3.7A 46.4 MOHM,,10,0,"
        "\"2,62800 zł\",\"26,28 zł\"\n";

    const auto result = parseDigiKeyCsvText(csv, {});
    assert(result.ok);
    assert(result.candidates.size() == 1);
    const auto& candidate = result.candidates.front();
    assert(candidate.item.digikeyPartNumber == "308-1571-1-ND");
    assert(candidate.item.sku == "CDMC6D28NP-4R7MC");
    assert(candidate.item.manufacturer == "Sumida America Components Inc.");
    assert(candidate.item.quantity == 10);
    assert(candidate.item.category == "Inductors");
    assert(candidate.item.parameters.size() == 4);
  }

  {
    const string csv =
        "Index,Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity,Unit Price,Extended Price\n"
        "1,399-C0603C105K4RACTUCT-ND,C0603C105K4RACTU,KEMET,CAP CER 1UF 16V X7R 0603,50,\"0,13420 zł\",\"6,71 zł\"\n";

    auto existing = makeSampleInventory();
    InventoryItem duplicate;
    duplicate.id = "existing-cap";
    duplicate.partName = "Existing Cap";
    duplicate.manufacturer = "KEMET";
    duplicate.category = "Capacitors";
    duplicate.quantity = 7;
    duplicate.digikeyPartNumber = "399-C0603C105K4RACTUCT-ND";
    duplicate.sku = "C0603C105K4RACTU";
    existing.push_back(duplicate);

    const auto result = parseDigiKeyCsvText(csv, existing);
    assert(result.ok);
    assert(result.candidates.size() == 1);
    assert(result.candidates.front().hasConflict);
    assert(result.candidates.front().existingItemId == "existing-cap");
    assert(result.candidates.front().matchedField == "DigiKey part");

    mergeImportedMetadata(duplicate, result.candidates.front().item);
    duplicate.quantity += result.candidates.front().item.quantity;
    assert(duplicate.quantity == 57);
  }

  {
    auto backend = make_unique<MockPrinterBackend>();
    auto* backendPtr = backend.get();
    LabelPrinterService service(move(backend));
    service.setConfiguredPrinter("ZDesigner LP 2824 Plus (ZPL)");
    const auto expectHeader = [&](const InventoryItem& candidate, const string& expected) {
      const auto plan = service.buildLabelPlan(candidate);
      if (plan.categoryHeader != expected) {
        cerr << "Expected header '" << expected << "' but got '" << plan.categoryHeader << "' for "
             << candidate.id << '\n';
      }
      assert(plan.categoryHeader == expected);
      if (expected.size() <= 16) {
        assert(service.buildZpl(candidate).find("^FD" + expected + "^FS") != string::npos);
      }
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
         "Created from a DigiKey code scan.", "Timer IC"},
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
    item.sku = "RC0603FR-0710KL";
    item.digikeyPartNumber = "311-10.0KHRCT-ND";
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
    assert(plan.scannerHint == "R-0002");
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
    assert(rackZpl.find("^FO10,173^A0N,18,18^FDR3-E3^FS") != string::npos);
    assert(zpl.find("^FDLA,0002^FS") != string::npos);
    assert(zpl.find("^FO170,175") == string::npos);
    assert(zpl.find("^FDR-0002^FS") == string::npos);
    assert(zpl.find("^FDInventatory^FS") == string::npos);
    assert(zpl.find("^FO5,0^GB246,24,24,B,6^FS") != string::npos);
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
    const auto tvsPlan = service.buildLabelPlan(tvsDiode);
    assert(tvsPlan.categoryHeader == "TVS Diode");
    assert(tvsPlan.parameterLine1.find("Vst") != string::npos);
    assert(tvsPlan.parameterLine2.find("Vc") != string::npos);
    assert(tvsPlan.parameterLine3.find("Ipp") != string::npos);
    assert(service.buildZpl(tvsDiode).find("^FDTVS Diode^FS") != string::npos);
    assert(service.buildZpl(tvsDiode).find("^FDVst 16V^FS") != string::npos);
    assert(service.buildZpl(tvsDiode).find("^FDVc 26V^FS") != string::npos);
    assert(service.buildZpl(tvsDiode).find("^FDIpp 23.1A^FS") != string::npos);

    InventoryItem protectionIc;
    protectionIc.id = "prot-ic-1";
    protectionIc.partName = "ESD protection array";
    protectionIc.manufacturer = "Nexperia";
    protectionIc.category = "Integrated Circuits";
    protectionIc.parameters = {{"Function", "Protection"}, {"Type", "ESD"}};
    const auto protectionPlan = service.buildLabelPlan(protectionIc);
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
                                   {"DigiKey Programmable", "Not Verified"},
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
    const auto opAmpPlan = service.buildLabelPlan(opAmp);
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
    const auto imuPlan = service.buildLabelPlan(imu);
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
      const auto genericPlan = service.buildLabelPlan(genericIc);
      assert(genericPlan.categoryHeader == "Integrated Circuit");
      assert(genericPlan.categoryHeader != "Memory IC");
    }

    InventoryItem fallback;
    fallback.id = "misc-1";
    fallback.partName = "Prototype module";
    fallback.manufacturer = "Acme";
    fallback.category = "Misc / Prototype";
    const auto fallbackPlan = service.buildLabelPlan(fallback);
    assert(fallbackPlan.categoryHeader == "Misc");
    assert(service.buildZpl(fallback).find("^FDMisc^FS") != string::npos);

    InventoryItem inductor;
    inductor.id = "ind-1";
    inductor.partName = "RF inductor";
    inductor.manufacturer = "Murata";
    inductor.category = "Inductors";
    inductor.notes = "FIXED IND 27NH 350MA 460 MOHM | DigiKey PN: 490-2628-1-ND";
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
    const string csv = "Digi-Key Part Number,Manufacturer,Description,Quantity\n"
                       "123-ND,Acme,Missing manufacturer part,3\n";
    const auto result = parseDigiKeyCsvText(csv, {});
    assert(!result.ok);
  }

  {
    const string csv = "Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity\n"
                       "123-ND,ABC-123,Acme,Overflow quantity,2147483648\n";
    const auto result = parseDigiKeyCsvText(csv, {});
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
        R"({"deviceId":"r1-a","requestId":"req-1","code":"[)>\u001e06\u001dP718-2362-1-ND\u001dQ2\u001e\u0004","quantity":2})",
        request, error));
    assert(request.quantity == 2);
    assert(request.code == string("[)>") + '\x1e' + "06" + '\x1d' + "P718-2362-1-ND" + '\x1d' + "Q2" + '\x1e' + '\x04');

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
        R"({"protocolVersion":1,"requestId":"sync-1","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":1,"capabilities":["lcd.128x64"],"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":["r1-a-76-result"]})",
        request, error));
    assert(request.protocolVersion == 1);
    assert(request.events.size() == 1);
    assert(request.events.front().value == 2);
    assert(request.resultAcks.size() == 1);
    assert(!request.hasLookup);
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-lookup","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-9","code":"0002"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.lookupId == "lookup-9");
    assert(request.lookup.code == "0002");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-digikey-lookup","deviceId":"r1-a","firmwareVersion":"0.5.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-dk-1","code":"718-2362-1-ND"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.code == "718-2362-1-ND");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-label","deviceId":"r1-a","firmwareVersion":"0.4.0","mode":"label_print","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"quickLabelPrint":{"requestId":"r1-a-label-1","presetIndex":2,"revision":3}})",
        request, error));
    assert(request.hasQuickLabelPrint);
    assert(request.quickLabelPrint.presetIndex == 2);
    assert(request.quickLabelPrint.revision == 3);
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-bad-lookup","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-10","code":"ABC"}})",
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
    quickLabelResponse.lookupResult = {"lookup-control", "found", string("part") + '\x01'};
    quickLabelResponse.hasLookupResult = true;
    assert(deviceSyncResponseJson(quickLabelResponse).find("part\\u0001") != string::npos);
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":2,"requestId":"sync-2","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[]})",
        request, error));

    const auto databasePath = filesystem::temp_directory_path() / "inventatory-device-sync-v1-test.db";
    filesystem::remove(databasePath);
    InventoryStore store;
    InventoryItem item;
    item.id = "sync-item";
    item.machineCode = "0002";
    item.partName = "10k resistor";
    item.digikeyPartNumber = "718-2362-1-ND";
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
    const auto digiKeyLookup = lookupDeviceItem(lookupSnapshot, {"lookup-dk-1", "718-2362-1-ND"});
    assert(digiKeyLookup.status == "found");
    assert(digiKeyLookup.itemName == "10k resistor");
    const auto databaseDigiKeyLookup =
        lookupDeviceItem(databasePath, {"lookup-dk-1-db", "718-2362-1-ND"});
    assert(databaseDigiKeyLookup.status == "found");
    assert(databaseDigiKeyLookup.itemName == "10k resistor");
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
        R"({"protocolVersion":1,"requestId":"sync-3","deviceId":"r1-a","firmwareVersion":"0.2.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":[]})",
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

    // A new protocol-v1 receive is auto-printed immediately after its event is
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
    expected.digiKeyClientId = "client-id";
    expected.digiKeyAccountId = "account-id";
    expected.digiKeySite = "PL";
    expected.digiKeyLanguage = "pl";
    expected.digiKeyCurrency = "PLN";
    assert(saveAppSettings(path, expected));

    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.schemaVersion == 2);
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
    assert(loaded.digiKeyClientId == "client-id");
    assert(loaded.digiKeyAccountId == "account-id");
    assert(loaded.digiKeySite == "PL");
    assert(loaded.digiKeyLanguage == "pl");
    assert(loaded.digiKeyCurrency == "PLN");

    ifstream persisted(path);
    const string text((istreambuf_iterator<char>(persisted)), istreambuf_iterator<char>());
    assert(text.find("client_secret") == string::npos);
    assert(text.find("secret") == string::npos);
    assert(text.find("device_service_port=8181") != string::npos);
    assert(text.find("bridge_port") == string::npos);
    assert(text.find("quick_label") == string::npos);
    persisted.close();
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    // Quick label presets travel with the Inventatory data folder rather than
    // the machine-local settings file, so they survive a restart even if the
    // data folder is the thing the user backs up or moves.
    const auto path = filesystem::temp_directory_path() / "inventatory-quick-labels-test.conf";
    error_code removeError;
    filesystem::remove(path, removeError);

    vector<string> missingPresets;
    uint32_t missingRevision = 1;
    assert(!loadQuickLabels(path, missingPresets, missingRevision));

    const vector<string> presets = {"5V", "GND", "12V"};
    assert(saveQuickLabels(path, presets, 9));

    vector<string> loadedPresets;
    uint32_t loadedRevision = 1;
    assert(loadQuickLabels(path, loadedPresets, loadedRevision));
    assert(loadedPresets == presets);
    assert(loadedRevision == 9);

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
    assert(loaded.schemaVersion == 2);
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

  // --- KiCad BOM integration -----------------------------------------------

  // A trimmed copy of a real KiCad grouped export: semicolon delimited, a
  // non-orderable REF** row, and an unescaped inch mark in "2.13" ePaper".
  const string kicadBom =
      "\"Id\";\"Designator\";\"Footprint\";\"Quantity\";\"Designation\";\"Supplier and ref\";\n"
      "1;\"C1, C15, C16, C22, C23, C25\";\"C_0603_1608Metric\";6;\"1uF\";;;\n"
      "2;\"C2, C4\";\"C_0603_1608Metric\";2;\"100nF\";;;\n"
      "3;\"C9\";\"C_0603_1608Metric\";1;\"0.01uF\";;;\n"
      "4;\"C19, C30\";\"CP_Radial_D5.0mm_P2.50mm\";2;\"100uF\";;;\n"
      "5;\"R1\";\"R_0603_1608Metric\";1;\"10K\";;;\n"
      "6;\"REF**, REF**, REF**, REF**\";\"TestPoint_Bridge_Pitch2.0mm_Drill0.7mm\";4;"
      "\"TestPoint_Bridge_Pitch2.0mm_Drill0.7mm\";;;\n"
      "7;\"U2\";\"JST_PH_B8B-PH-K_1x08_P2.00mm_Vertical\";1;\"2.13\" ePaper\";;;\n"
      "8;\"U3\";\"ESP32-S3-WROOM-1\";1;\"ESP32-S3-WROOM-1\";;;\n";

  {
    assert(sniffDelimiter(kicadBom) == ';');
    assert(sniffDelimiter("Digi-Key Part Number,Quantity\n1276-1000-1-ND,25\n") == ',');
    // A quoted comma inside a semicolon file must not swing the vote.
    assert(sniffDelimiter("\"a;b\";\"2,62800\";\"c\"\n") == ';');
    assert(stripByteOrderMark("\xEF\xBB\xBFId") == "Id");
  }

  {
    assert(detectCsvFormat(kicadBom) == CsvFormat::KicadBom);
    assert(detectCsvFormat(
               "Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity\n"
               "1276-1000-1-ND,CL10B104KB8NNNC,Samsung,CAP CER 100NF 50V X7R 0603,25\n") ==
           CsvFormat::DigiKeyOrder);
    assert(detectCsvFormat("alpha,beta\n1,2\n") == CsvFormat::Unknown);
    assert(detectCsvFormat("") == CsvFormat::Unknown);
  }

  {
    const auto bom = parseKicadBomText(kicadBom, "Astro Arrow");
    assert(bom.ok);
    assert(bom.projectName == "Astro Arrow");
    // Seven orderable rows: the REF** test-point row is excluded.
    assert(bom.lines.size() == 7);
    assert(bom.warnings.size() == 1);
    assert(bom.lines.front().designators.size() == 6);
    assert(bom.lines.front().designators.front() == "C1");
    assert(bom.lines.front().designators.back() == "C25");
    assert(bom.lines.front().quantityPerBoard == 6);
    assert(bom.lines.front().designation == "1uF");
    assert(bom.lines.front().footprint == "C_0603_1608Metric");
    // Lenient quote recovery keeps the display row intact instead of swallowing
    // the rest of the file.
    const auto epaper = find_if(bom.lines.begin(), bom.lines.end(), [](const BomLine& line) {
      return !line.designators.empty() && line.designators.front() == "U2";
    });
    assert(epaper != bom.lines.end());
    assert(epaper->designation == "2.13\" ePaper");
    assert(epaper->quantityPerBoard == 1);

    assert(isNonOrderableDesignator("REF**", "TestPoint_Bridge"));
    assert(isNonOrderableDesignator("H1", "MountingHole_3.2mm"));
    assert(isNonOrderableDesignator("TP4", ""));
    assert(!isNonOrderableDesignator("U3", "ESP32-S3-WROOM-1"));
    assert(!isNonOrderableDesignator("HDR1", "PinHeader_1x04"));

    assert(projectNameFromPath("C:/tmp/Astro_Arrow_PCB_R1_2.1.csv") == "Astro Arrow PCB R1 2.1");
  }

  {
    const auto value = [](const string& text, ValueKind expected) {
      ValueKind kind = ValueKind::None;
      const auto parsed = parseElectricalValue(text, kind);
      assert(parsed.has_value());
      assert(kind == expected);
      return *parsed;
    };
    const auto near = [](double lhs, double rhs) { return fabs(lhs - rhs) <= fabs(rhs) * 1e-9 + 1e-18; };

    assert(near(value("1uF", ValueKind::Capacitance), 1e-6));
    assert(near(value("100nF", ValueKind::Capacitance), 1e-7));
    assert(near(value("0.01uF", ValueKind::Capacitance), 1e-8));
    assert(near(value("330nF", ValueKind::Capacitance), 3.3e-7));
    assert(near(value("0.47uF", ValueKind::Capacitance), 4.7e-7));
    assert(near(value("22uF", ValueKind::Capacitance), 2.2e-5));
    assert(near(value("10K", ValueKind::Resistance), 1e4));
    assert(near(value("500k", ValueKind::Resistance), 5e5));
    assert(near(value("2M", ValueKind::Resistance), 2e6));
    assert(near(value("280R", ValueKind::Resistance), 280.0));
    assert(near(value("270 ohm", ValueKind::Resistance), 270.0));
    // Leading R is RKM notation for a sub-ohm value.
    assert(near(value("R280", ValueKind::Resistance), 0.28));
    assert(near(value("4R7", ValueKind::Resistance), 4.7));
    assert(near(value("4.7uH", ValueKind::Inductance), 4.7e-6));
    assert(near(value("32.768KHz", ValueKind::Frequency), 32768.0));

    ValueKind kind = ValueKind::None;
    assert(!parseElectricalValue("ESP32-S3-WROOM-1", kind).has_value());
    assert(!parseElectricalValue("RGBW_Strip", kind).has_value());
    assert(!parseElectricalValue("", kind).has_value());

    assert(looksLikePartNumber("ESP32-S3-WROOM-1"));
    assert(looksLikePartNumber("AP63203WU"));
    assert(!looksLikePartNumber("100nF"));
    assert(!looksLikePartNumber("10K"));
  }

  {
    assert(packageFromFootprint("C_0603_1608Metric") == "0603");
    assert(packageFromFootprint("R_0603_1608Metric") == "0603");
    assert(packageFromFootprint("LED_0603_1608Metric_Pad1.05x0.95mm_HandSolder") == "0603");
    assert(packageFromFootprint("Fuse_1206_3216Metric") == "1206");
    assert(packageFromFootprint("TSOT-23-6") == "TSOT-23-6");
    assert(packageFromFootprint("SOIC-8_3.9x4.9mm_P1.27mm") == "SOIC-8");
    assert(packageFromFootprint("HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm") == "HTSSOP-16-1EP");
    assert(packageFromFootprint("CP_Radial_D5.0mm_P2.50mm") == "Radial 5.0mm");
    assert(packageFromFootprint("JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical") == "JST PH 3");
    assert(packageFromFootprint("") == "");

    assert(packageMatches("0603", "0603 (1608 Metric)"));
    assert(!packageMatches("0603", "0805"));
    assert(packageMatches("SOT-23-6", "SOT-23-6"));
    // Short tokens must not match by substring, or "TO" would swallow the world.
    assert(!packageMatches("TO", "TO-220-3"));
    assert(!packageMatches("0603", ""));
  }

  {
    vector<InventoryItem> items;

    InventoryItem cap;
    cap.id = "cap-100nf-0603";
    cap.partName = "CAP CER 100NF 50V X7R 0603";
    cap.category = "Capacitors";
    cap.quantity = 340;
    cap.parameters = {{"Capacitance", "100 nF"}, {"Package / Case", "0603 (1608 Metric)"}};
    items.push_back(cap);

    InventoryItem wrongPackage = cap;
    wrongPackage.id = "cap-100nf-0805";
    wrongPackage.partName = "CAP CER 100NF 50V X7R 0805";
    wrongPackage.quantity = 9000;
    wrongPackage.parameters = {{"Capacitance", "100 nF"}, {"Package / Case", "0805 (2012 Metric)"}};
    items.push_back(wrongPackage);

    InventoryItem noPackage = cap;
    noPackage.id = "cap-100nf-unknown";
    noPackage.partName = "Bulk 100nF";
    noPackage.quantity = 5;
    noPackage.parameters = {{"Capacitance", "100 nF"}};
    items.push_back(noPackage);

    InventoryItem module;
    module.id = "esp32-module";
    module.partName = "IC RF TXRX+MCU MODULE";
    module.category = "Integrated Circuits";
    module.quantity = 7;
    module.sku = "ESP32-S3-WROOM-1-N16R8";
    items.push_back(module);

    InventoryItem exactModule;
    exactModule.id = "esp32-exact";
    exactModule.partName = "Espressif module";
    exactModule.category = "Integrated Circuits";
    exactModule.quantity = 1;
    exactModule.sku = "ESP32-S3-WROOM-1";
    items.push_back(exactModule);

    const auto bom = parseKicadBomText(kicadBom, "Astro Arrow");
    auto analysis = analyzeBom(bom, items, 1, {});
    assert(analysis.lines.size() == analysis.matches.size());

    const auto lineIndexOf = [&](const string& designation) {
      for (size_t index = 0; index < analysis.lines.size(); ++index) {
        if (analysis.lines[index].designation == designation) return index;
      }
      assert(false);
      return size_t{0};
    };

    // Value plus package beats value alone, even though the 0805 reel is far
    // larger and would otherwise win the tie-break.
    const auto capIndex = lineIndexOf("100nF");
    assert(analysis.matches[capIndex].chosenItemId() == "cap-100nf-0603");
    assert(analysis.matches[capIndex].needed == 2);
    assert(analysis.matches[capIndex].available == 340);
    assert(analysis.matches[capIndex].sufficient);
    // The 0805 part is rejected outright; only the 0603 and the package-less
    // fallback are offered as alternates.
    assert(analysis.matches[capIndex].candidates.size() == 2);

    // An exact SKU match outranks a substring hit on a bigger reel.
    const auto moduleIndex = lineIndexOf("ESP32-S3-WROOM-1");
    assert(analysis.matches[moduleIndex].chosenItemId() == "esp32-exact");

    // Nothing in stock is a 100uF radial, so that line is a shortage.
    const auto radialIndex = lineIndexOf("100uF");
    assert(analysis.matches[radialIndex].candidates.empty());
    assert(!analysis.matches[radialIndex].sufficient);
    assert(analysis.shortCount > 0);

    // A line needing more than the shelf holds is short.
    const auto oneMicroIndex = lineIndexOf("1uF");
    assert(!analysis.matches[oneMicroIndex].sufficient);

    // Overrides pin the alternate across a re-analysis.
    map<string, string> overrides;
    overrides[bomLineKey(analysis.lines[capIndex])] = "cap-100nf-unknown";
    const auto pinned = analyzeBom(bom, items, 1, overrides);
    assert(pinned.matches[capIndex].chosenItemId() == "cap-100nf-unknown");

    // Board count scales every requirement and can flip a line into shortage.
    const auto doubled = analyzeBom(bom, items, 2, {});
    assert(doubled.boards == 2);
    for (size_t index = 0; index < doubled.matches.size(); ++index) {
      assert(doubled.matches[index].needed == analysis.matches[index].needed * 2);
    }
    const auto exactIndex = lineIndexOf("ESP32-S3-WROOM-1");
    assert(analysis.matches[exactIndex].sufficient);   // 1 needed, 1 in stock
    assert(!doubled.matches[exactIndex].sufficient);   // 2 needed, 1 in stock
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-bom-projects-test.db";
    error_code cleanupError;
    filesystem::remove(path, cleanupError);

    BomProject project;
    project.id = "bom-roundtrip";
    project.name = "Astro Arrow PCB R1 2.1";
    project.sourcePath = "C:/tmp/Astro_Arrow_PCB_R1_2.1.csv";
    project.boards = 3;
    project.createdAt = 1710000000;
    project.lastOpened = 1710000100;
    project.lastBuilt = 1710000200;
    project.bomText = kicadBom;  // multi-line, quoted, semicolon delimited
    project.overrides = {{"100nf|c06031608metric", "cap-100nf-0603"}, {"a=b;c", "weird|id"}};
    project.enrichment = {{"100uf|cpradiald50mmp250mm", "493-13399-ND"}};

    assert(saveBomProjects(path, {project}));
    vector<BomProject> loaded;
    assert(loadBomProjects(path, loaded));
    assert(loaded.size() == 1);
    assert(loaded.front().id == project.id);
    assert(loaded.front().name == project.name);
    assert(loaded.front().boards == 3);
    assert(loaded.front().lastBuilt == 1710000200);
    assert(loaded.front().bomText == kicadBom);
    assert(loaded.front().overrides == project.overrides);
    assert(loaded.front().enrichment == project.enrichment);

    // Saving is a full snapshot, so an empty list clears the table.
    assert(saveBomProjects(path, {}));
    vector<BomProject> empty;
    assert(loadBomProjects(path, empty));
    assert(empty.empty());

    filesystem::remove(path, cleanupError);
  }

  cout << "Inventatory core tests passed\n";
  return 0;
}
