#include "core/Inventory.h"
#include "App.h"
#include "app/AppBootstrap.h"
#include "app/AppSettings.h"
#include "platform/UpdateService.h"
#include "platform/StartupRegistration.h"
#include "platform/Environment.h"
#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"
#include "core/InventoryTransfer.h"
#ifdef near
#undef near
#endif
#include "core/InventatoryScanProtocol.h"
#include "platform/HttpServer.h"
#include "platform/CredentialStore.h"
#ifdef near
#undef near
#endif
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
#include <array>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>
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

string sendLocalHttpRequest(uint16_t port, const string& request, DWORD receiveTimeout = 3000) {
  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(client != INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  assert(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR);
  assert(send(client, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()));

  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
  string response;
  array<char, 1024> buffer{};
  int received = 0;
  while ((received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0)) > 0) {
    response.append(buffer.data(), static_cast<size_t>(received));
  }
  closesocket(client);
  return response;
}

SOCKET connectSlowLocalClient(uint16_t port) {
  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(client != INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  assert(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR);
  const string partialRequest = "POST /api/v1/device/sync HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  assert(send(client, partialRequest.data(), static_cast<int>(partialRequest.size()), 0) ==
         static_cast<int>(partialRequest.size()));
  return client;
}

string signedSyncRequest(const string& token, const string& deviceId, uint64_t counter, const string& body) {
  ostringstream request;
  request << "POST /api/v1/device/sync HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n";
  request << "Content-Length: " << body.size() << "\r\n";
  request << "X-Inventatory-Protocol: " << kInventatoryScanTransportProtocolVersion << "\r\n";
  request << "X-Inventatory-Device: " << deviceId << "\r\n";
  request << "X-Inventatory-Counter: " << counter << "\r\n";
  request << "X-Inventatory-Mac: "
          << deviceRequestMac(token, "POST", "/api/v1/device/sync", deviceId, counter, body) << "\r\n\r\n";
  request << body;
  return request.str();
}

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

// Forward declarations for physical value tests
void testPhysicalValueParsing();
void testPhysicalValueMatching();
void testPhysicalValueSearchIntegration();

// Physical value parsing and matching tests
void testPhysicalValueParsing() {
  // Capacitance values
  auto cap1 = parsePhysicalValue("100nF");
  assert(cap1.has_value());
  assert(cap1->type == PhysicalValueType::Capacitance);
  assert(std::abs(cap1->value - 1e-7) < 1e-15);

  auto cap2 = parsePhysicalValue("0.1uF");
  assert(cap2.has_value());
  assert(cap2->type == PhysicalValueType::Capacitance);
  assert(std::abs(cap2->value - 1e-7) < 1e-15);

  const auto unicodeMicroFarad = string("0.1 ") + "\xC2\xB5" + "F";
  auto capUnicode = parsePhysicalValue(unicodeMicroFarad);
  assert(capUnicode.has_value());
  assert(capUnicode->type == PhysicalValueType::Capacitance);
  assert(std::abs(capUnicode->value - 1e-7) < 1e-15);

  auto cap3 = parsePhysicalValue("1uF");
  assert(cap3.has_value());
  assert(cap3->type == PhysicalValueType::Capacitance);
  assert(std::abs(cap3->value - 1e-6) < 1e-15);

  auto cap4 = parsePhysicalValue("10uF");
  assert(cap4.has_value());
  assert(cap4->type == PhysicalValueType::Capacitance);
  assert(std::abs(cap4->value - 1e-5) < 1e-15);

  auto cap5 = parsePhysicalValue("100pF");
  assert(cap5.has_value());
  assert(cap5->type == PhysicalValueType::Capacitance);
  assert(std::abs(cap5->value - 1e-10) < 1e-15);

  // Resistance values
  auto res1 = parsePhysicalValue("10k Ohm");
  assert(res1.has_value());
  assert(res1->type == PhysicalValueType::Resistance);
  assert(std::abs(res1->value - 10000.0) < 0.01);

  auto res2 = parsePhysicalValue("4R7");
  assert(res2.has_value());
  assert(res2->type == PhysicalValueType::Resistance);
  assert(std::abs(res2->value - 4.7) < 0.01);

  auto res3 = parsePhysicalValue("100 Ohm");
  assert(res3.has_value());
  assert(res3->type == PhysicalValueType::Resistance);
  assert(std::abs(res3->value - 100.0) < 0.01);

  auto res4 = parsePhysicalValue("1M");
  assert(res4.has_value());
  assert(res4->type == PhysicalValueType::Resistance);
  assert(std::abs(res4->value - 1000000.0) < 1.0);

  // Inductance values
  auto ind1 = parsePhysicalValue("4.7uH");
  assert(ind1.has_value());
  assert(ind1->type == PhysicalValueType::Inductance);
  assert(std::abs(ind1->value - 4.7e-6) < 1e-10);

  auto ind2 = parsePhysicalValue("10mH");
  assert(ind2.has_value());
  assert(ind2->type == PhysicalValueType::Inductance);
  assert(std::abs(ind2->value - 0.01) < 0.0001);

  // Frequency values
  auto freq1 = parsePhysicalValue("1MHz");
  assert(freq1.has_value());
  assert(freq1->type == PhysicalValueType::Frequency);
  assert(std::abs(freq1->value - 1e6) < 1.0);

  auto freq2 = parsePhysicalValue("100kHz");
  assert(freq2.has_value());
  assert(freq2->type == PhysicalValueType::Frequency);
  assert(std::abs(freq2->value - 100000.0) < 0.1);

  // Case insensitive
  auto capUpper = parsePhysicalValue("100NF");
  assert(capUpper.has_value());
  assert(capUpper->type == PhysicalValueType::Capacitance);

  // Invalid inputs
  assert(!parsePhysicalValue("").has_value());
  assert(!parsePhysicalValue("abc").has_value());
  assert(!parsePhysicalValue("123").has_value());
}

void testPhysicalValueMatching() {
  // 100nF should match 0.1uF (same value, different prefix)
  const auto exactCap = comparePhysicalValues("100nF", "0.1uF");
  assert(exactCap.has_value());
  assert(exactCap->band == PhysicalValueMatchBand::Exact);
  const auto unicodeCap = comparePhysicalValues("100nF", string("0.1 ") + "\xC2\xB5" + "F");
  assert(unicodeCap.has_value());
  assert(unicodeCap->band == PhysicalValueMatchBand::Exact);

  // 10k Ohm should match 10000 Ohm
  assert(comparePhysicalValues("10k Ohm", "10000 Ohm")->band == PhysicalValueMatchBand::Exact);

  // 4R7 should match 4.7 Ohm
  assert(comparePhysicalValues("4R7", "4.7 Ohm")->band == PhysicalValueMatchBand::Exact);

  // 4.7uH should match 4700nH
  assert(comparePhysicalValues("4.7uH", "4700nH")->band == PhysicalValueMatchBand::Exact);

  // 1MHz should match 1000kHz
  assert(comparePhysicalValues("1MHz", "1000kHz")->band == PhysicalValueMatchBand::Exact);

  // Different types should not match
  assert(!comparePhysicalValues("100nF", "100 Ohm").has_value());

  // The built-in bands classify 20% as possible and reject values beyond 25%.
  assert(comparePhysicalValues("120nF", "100nF")->band == PhysicalValueMatchBand::Possible);
  assert(comparePhysicalValues("125nF", "100nF")->band == PhysicalValueMatchBand::Possible);
  assert(comparePhysicalValues("126nF", "100nF")->band == PhysicalValueMatchBand::None);

  // Within the workable band should match
  assert(comparePhysicalValues("101nF", "100nF")->band == PhysicalValueMatchBand::Workable);
}

void testPhysicalValueSearchIntegration() {
  vector<InventoryItem> items;

  // Item with 0.1uF capacitance
  InventoryItem cap1;
  cap1.id = "cap-01uf";
  cap1.partName = "0.1uF Capacitor";
  cap1.category = "Capacitors";
  cap1.quantity = 50;
  cap1.parameters = {{"Capacitance", string("0.1 ") + "\xC2\xB5" + "F"}, {"Voltage", "50V"}};
  items.push_back(cap1);

  // Item with 100nF capacitance (physically same as 0.1uF)
  InventoryItem cap2;
  cap2.id = "cap-100nf";
  cap2.partName = "100nF Capacitor";
  cap2.category = "Capacitors";
  cap2.quantity = 100;
  cap2.parameters = {{"Capacitance", "100nF"}, {"Voltage", "50V"}};
  items.push_back(cap2);

  // Item with 1uF capacitance (different value)
  InventoryItem cap3;
  cap3.id = "cap-1uf";
  cap3.partName = "1uF Capacitor";
  cap3.category = "Capacitors";
  cap3.quantity = 25;
  cap3.parameters = {{"Capacitance", "1uF"}, {"Voltage", "16V"}};
  items.push_back(cap3);

  // Item with 10k Ohm resistance
  InventoryItem res1;
  res1.id = "res-10k";
  res1.partName = "10k Resistor";
  res1.category = "Resistors";
  res1.quantity = 200;
  res1.parameters = {{"Resistance", "10k Ohm"}, {"Tolerance", "1%"}};
  items.push_back(res1);

  // Item with 10000 Ohm resistance (physically same as 10k)
  InventoryItem res2;
  res2.id = "res-10000";
  res2.partName = "10000 Ohm Resistor";
  res2.category = "Resistors";
  res2.quantity = 150;
  res2.parameters = {{"Resistance", "10000 Ohm"}, {"Tolerance", "1%"}};
  items.push_back(res2);

  // Search for "100nF" should find both cap1 and cap2
  auto capFiltered = filterItems(items, "100nF");
  assert(capFiltered.size() >= 2);
  bool foundCap1 = false, foundCap2 = false;
  for (size_t idx : capFiltered) {
    if (items[idx].id == "cap-01uf") foundCap1 = true;
    if (items[idx].id == "cap-100nf") foundCap2 = true;
  }
  assert(foundCap1);
  assert(foundCap2);

  // Search for "0.1uF" should find both cap1 and cap2
  capFiltered = filterItems(items, "0.1uF");
  assert(capFiltered.size() >= 2);
  foundCap1 = false;
  foundCap2 = false;
  for (size_t idx : capFiltered) {
    if (items[idx].id == "cap-01uf") foundCap1 = true;
    if (items[idx].id == "cap-100nf") foundCap2 = true;
  }
  assert(foundCap1);
  assert(foundCap2);

  // Search for "10k" should find both res1 and res2
  auto resFiltered = filterItems(items, "10k");
  assert(resFiltered.size() >= 2);
  bool foundRes1 = false, foundRes2 = false;
  for (size_t idx : resFiltered) {
    if (items[idx].id == "res-10k") foundRes1 = true;
    if (items[idx].id == "res-10000") foundRes2 = true;
  }
  assert(foundRes1);
  assert(foundRes2);

  // param: prefix should also work with physical values
  auto paramFiltered = filterItems(items, "param:Capacitance=0.1uF");
  assert(paramFiltered.size() >= 2);
  foundCap1 = false;
  foundCap2 = false;
  for (size_t idx : paramFiltered) {
    if (items[idx].id == "cap-01uf") foundCap1 = true;
    if (items[idx].id == "cap-100nf") foundCap2 = true;
  }
  assert(foundCap1);
  assert(foundCap2);

  const auto ranked = rankedFilterItems(items, "101nF", {}, 5);
  assert(ranked.size() == 2);
  assert(ranked[0].band == PhysicalValueMatchBand::Workable);
  assert(ranked[1].band == PhysicalValueMatchBand::Workable);
  assert(ranked[0].relativeDifference <= ranked[1].relativeDifference + 1e-12);

  const auto closest = findClosestPhysicalValues(items, "9.9k");
  assert(closest.size() == 2);
  assert(items[closest[0].itemIndex].id == "res-10k");
  assert(closest[0].band == PhysicalValueMatchBand::Workable);
}

void testInventoryCommitHistory() {
#ifdef _WIN32
  const auto path = filesystem::temp_directory_path() / "inventatory-inventory-commit-history-test.db";
  error_code cleanupError;
  filesystem::remove(path, cleanupError);

  InventoryStore baseline;
  InventoryItem item;
  item.id = "vc-item";
  item.partName = "Versioned resistor";
  item.manufacturer = "Acme";
  item.category = "Resistors";
  item.quantity = 10;
  item.lastUpdated = 1710000000;
  item.tags = {"smd", "production"};
  item.parameters = {{"Resistance", "10k"}};
  item.vendorMetadata.provider = "digikey";
  item.vendorMetadata.categoryPath = {"Resistors", "Chip Resistor"};
  baseline.items().push_back(item);

  InventoryItem removed;
  removed.id = "vc-removed";
  removed.partName = "Removed part";
  removed.quantity = 2;
  removed.lastUpdated = 1710000000;
  baseline.items().push_back(removed);

  InventatoryRack rack;
  rack.id = "vc-rack";
  rack.code = "R1";
  rack.componentType = "Resistors";
  rack.rows = 4;
  rack.columns = 6;
  baseline.racks().push_back(rack);

  ensureInventoryIdentifiers(baseline.items());
  reconcileRackAssignments(baseline);
  assert(baseline.save(path));
  assert(ensureInventoryCommitHistory(path, baseline));
  vector<InventoryCommit> commits;
  assert(loadInventoryCommits(path, commits));
  assert(commits.size() == 1);
  assert(commits.front().sequence == 1);
  assert(commits.front().parentId.empty());
  assert(commits.front().message == "Initial inventory");

  InventoryCommitDraft noOpDraft;
  noOpDraft.source = "manual";
  noOpDraft.message = "Should not be written";
  InventoryCommit noOpCommit;
  assert(baseline.saveWithCommit(path, baseline, noOpDraft, {}, nullptr, &noOpCommit));
  assert(noOpCommit.id.empty());
  assert(loadInventoryCommits(path, commits) && commits.size() == 1);

  InventoryStore changed = baseline;
  changed.items().front().quantity = 14;
  changed.items().front().notes = "Placed on the production line";
  changed.items().front().tags.push_back("priority");
  changed.items().front().parameters.push_back({"Tolerance", "1%"});
  changed.items().front().vendorMetadata.categoryPath.push_back("Precision");
  changed.items().front().rackId = rack.id;
  changed.items().front().rackSlot = "A1";
  changed.items().front().rackAssignment = RackAssignmentMode::Manual;
  changed.items().erase(changed.items().begin() + 1);
  InventoryItem added;
  added.id = "vc-added";
  added.partName = "Added capacitor";
  added.quantity = 6;
  added.lastUpdated = 1710000000;
  changed.items().push_back(added);
  ensureInventoryIdentifiers(changed.items());
  reconcileRackAssignments(changed);
  changed.racks().front().componentType = "Precision resistors";
  changed.racks().front().rows = 5;

  const auto changes = inventoryCommitDiff(baseline, changed);
  assert(!changes.empty());
  assert(any_of(changes.begin(), changes.end(), [](const InventoryFieldChange& change) {
    return change.entityType == "item" && change.entityId == "vc-item" && change.field == "quantity" &&
           change.before == "10" && change.after == "14";
  }));
  assert(any_of(changes.begin(), changes.end(), [](const InventoryFieldChange& change) {
    return change.entityType == "item" && change.entityId == "vc-item" && change.field == "tags";
  }));
  assert(any_of(changes.begin(), changes.end(), [](const InventoryFieldChange& change) {
    return change.entityType == "item" && change.entityId == "vc-removed" && change.field == "record";
  }));
  assert(any_of(changes.begin(), changes.end(), [](const InventoryFieldChange& change) {
    return change.entityType == "rack" && change.entityId == "vc-rack" && change.field == "rows";
  }));

  InventoryCommitDraft changeDraft;
  changeDraft.source = "import";
  changeDraft.reference = "order-42";
  InventoryCommit changedCommit;
  assert(changed.saveWithCommit(path, baseline, changeDraft, {}, nullptr, &changedCommit));
  assert(changedCommit.sequence == 2);
  assert(changedCommit.parentId == commits.front().id);
  assert(changedCommit.changedItemCount == 3);
  assert(changedCommit.changedRackCount == 1);
  assert(changedCommit.message.find("Imported inventory [order-42]") != string::npos);
  assert(changedCommit.message.find("3 parts, 1 rack") != string::npos);

  InventoryCommitDetail detail;
  assert(loadInventoryCommit(path, changedCommit.id, detail));
  assert(detail.hasParent);
  assert(inventoryCommitDiff(detail.snapshot, changed).empty());
  assert(inventoryCommitDiff(detail.parentSnapshot, baseline).empty());

  InventoryStore reversed;
  string conflict;
  assert(prepareInventoryCommitReverse(detail, changed, reversed, conflict));
  assert(inventoryCommitDiff(reversed, baseline).empty());

  InventoryStore conflicting = changed;
  conflicting.items().front().notes = "Changed later";
  InventoryStore untouched;
  assert(!prepareInventoryCommitReverse(detail, conflicting, untouched, conflict));
  assert(!conflict.empty());
  assert(inventoryCommitDiff(conflicting, changed).size() == 1);

  InventoryCommitDraft correctiveDraft;
  correctiveDraft.source = "revert";
  correctiveDraft.corrective = true;
  correctiveDraft.revertedCommitId = changedCommit.id;
  correctiveDraft.message = "Reversed changes from commit #2";
  InventoryCommit reverseCommit;
  assert(reversed.saveWithCommit(path, changed, correctiveDraft, {}, nullptr, &reverseCommit));
  assert(reverseCommit.corrective);
  assert(reverseCommit.revertedCommitId == changedCommit.id);
  assert(reverseCommit.sequence == 3);

  InventoryStore later = baseline;
  later.items().front().location = "Production shelf";
  InventoryCommitDraft laterDraft;
  laterDraft.source = "manual";
  laterDraft.message = "Updated location";
  assert(later.saveWithCommit(path, baseline, laterDraft));
  InventoryCommitDetail selectedDetail;
  assert(loadInventoryCommit(path, changedCommit.id, selectedDetail));
  InventoryCommitDraft restoreDraft;
  restoreDraft.source = "revert";
  restoreDraft.corrective = true;
  restoreDraft.revertedCommitId = changedCommit.id;
  restoreDraft.message = "Restored snapshot from commit #2";
  InventoryCommit restoreCommit;
  assert(selectedDetail.snapshot.saveWithCommit(path, later, restoreDraft, {}, nullptr, &restoreCommit));
  InventoryStore restored;
  assert(restored.load(path));
  assert(inventoryCommitDiff(restored, selectedDetail.snapshot).empty());

  InventoryCommitDraft checkpointDraft;
  checkpointDraft.source = "checkpoint";
  checkpointDraft.checkpoint = true;
  checkpointDraft.message = "Before assembly";
  InventoryCommit checkpoint;
  assert(restored.saveWithCommit(path, restored, checkpointDraft, {}, nullptr, &checkpoint));
  assert(checkpoint.checkpoint);
  assert(checkpoint.changedItemCount == 0 && checkpoint.changedRackCount == 0);
  assert(loadInventoryCommits(path, commits));
  assert(commits.size() == 6);
  assert(commits.front().sequence == 6);
  assert(commits.front().checkpoint);
  assert(commits.back().sequence == 1);

  filesystem::remove(path, cleanupError);
  assert(!cleanupError);
#endif
}

void testSqliteSchemaMigrationAndValidation() {
#ifdef _WIN32
  const auto legacyPath = filesystem::temp_directory_path() / "inventatory-legacy-schema-test.db";
  const auto invalidPath = filesystem::temp_directory_path() / "inventatory-invalid-schema-test.db";
  error_code cleanupError;
  filesystem::remove(legacyPath, cleanupError);
  filesystem::remove(invalidPath, cleanupError);
  const auto readBytes = [](const filesystem::path& path) {
    ifstream input(path, ios::binary);
    return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  };
  const auto readUserVersion = [](SqliteConnection& connection) {
    SqliteStatement statement;
    assert(sqliteApi().prepare_v2(connection.db, "PRAGMA user_version", -1, &statement.stmt, nullptr) == SQLITE_OK);
    assert(sqliteApi().step(statement.stmt) == SQLITE_ROW);
    assert(sqliteApi().column_type(statement.stmt, 0) == SQLITE_INTEGER);
    return sqliteApi().column_int64(statement.stmt, 0);
  };
  const auto readSchema = [](SqliteConnection& connection) {
    SqliteStatement statement;
    assert(sqliteApi().prepare_v2(
               connection.db,
               "SELECT name, sql FROM sqlite_master WHERE type IN ('table', 'index') ORDER BY name",
               -1, &statement.stmt, nullptr) == SQLITE_OK);
    string schema;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
      if (!schema.empty()) schema += '|';
      schema += sqliteText(statement.stmt, 0) + ':' + sqliteText(statement.stmt, 1);
    }
    assert(stepResult == SQLITE_DONE);
    return schema;
  };

  {
    SqliteConnection connection;
    assert(openDatabase(legacyPath, connection));
    assert(execSql(connection, R"SQL(
      CREATE TABLE inventatory_items (
        id TEXT PRIMARY KEY, part_name TEXT NOT NULL, manufacturer TEXT NOT NULL, category TEXT NOT NULL,
        quantity INTEGER NOT NULL, reorder_threshold INTEGER NOT NULL, location TEXT NOT NULL,
        tags TEXT NOT NULL, parameters TEXT NOT NULL, notes TEXT NOT NULL,
        manufacturer_part_number TEXT NOT NULL, datasheet_url TEXT NOT NULL, enrichment_status TEXT NOT NULL,
        last_updated INTEGER NOT NULL, inventatory_id TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL DEFAULT 0,
        machine_code TEXT NOT NULL DEFAULT '', rack_id TEXT NOT NULL DEFAULT '', rack_slot TEXT NOT NULL DEFAULT '',
        rack_assignment TEXT NOT NULL DEFAULT 'automatic'
      );
      INSERT INTO inventatory_items
        (id, part_name, manufacturer, category, quantity, reorder_threshold, location, tags, parameters, notes,
         manufacturer_part_number, datasheet_url, enrichment_status, last_updated)
      VALUES ('legacy-1', 'Legacy resistor', 'Acme', 'Resistors', 4, 1, '', '', '', '', 'LEGACY-SKU', '', 'synced', 1710000000);
    )SQL"));
    assert(ensureInventoryDatabaseSchema(connection));
  }
  {
    const auto beforeBytes = readBytes(legacyPath);
    {
      SqliteConnection connection;
      assert(openDatabaseReadOnly(legacyPath, connection));
      const auto beforeVersion = readUserVersion(connection);
      const auto beforeSchema = readSchema(connection);
      string error;
      assert(validateInventoryDatabase(connection, &error));
      assert(readUserVersion(connection) == beforeVersion);
      assert(readSchema(connection) == beforeSchema);
      {
        SqliteStatement statement;
        assert(sqliteApi().prepare_v2(connection.db, "SELECT sku FROM inventatory_items WHERE id='legacy-1'", -1,
                                      &statement.stmt, nullptr) == SQLITE_OK);
        assert(sqliteApi().step(statement.stmt) == SQLITE_ROW);
        assert(sqliteText(statement.stmt, 0) == "LEGACY-SKU");
      }
    }
    assert(readBytes(legacyPath) == beforeBytes);
  }
  {
    SqliteConnection connection;
    assert(openDatabase(legacyPath, connection));
    assert(execSql(connection, "UPDATE inventatory_items SET quantity=2147483648 WHERE id='legacy-1'"));
    string error;
    assert(!ensureInventoryDatabaseSchema(connection, &error));
    assert(readUserVersion(connection) == kInventoryDatabaseSchemaVersion);
    assert(execSql(connection, "UPDATE inventatory_items SET quantity=4, last_updated=-1 WHERE id='legacy-1'"));
    assert(!ensureInventoryDatabaseSchema(connection, &error));
    assert(execSql(connection, "UPDATE inventatory_items SET last_updated=1710000000, quantity='invalid' WHERE id='legacy-1'"));
    assert(!ensureInventoryDatabaseSchema(connection, &error));
  }

  const auto duplicatePath = filesystem::temp_directory_path() / "inventatory-duplicate-identifiers-test.db";
  filesystem::remove(duplicatePath, cleanupError);
  {
    InventoryStore duplicate;
    duplicate.items().push_back({"same-id", "First", "Acme", "Resistors", 1});
    duplicate.items().push_back({"same-id", "Second", "Acme", "Resistors", 1});
    assert(!duplicate.save(duplicatePath));
  }
  filesystem::remove(duplicatePath, cleanupError);

  {
    const auto beforeBytes = readBytes(invalidPath);
    string migratedBytes;
    {
      SqliteConnection connection;
      assert(openDatabase(invalidPath, connection));
      assert(execSql(connection, "CREATE TABLE inventatory_items (id TEXT PRIMARY KEY)"));
      assert(execSql(connection, "INSERT INTO inventatory_items (id) VALUES ('preserved-row')"));
      migratedBytes = readBytes(invalidPath);
      const auto beforeVersion = readUserVersion(connection);
      const auto beforeSchema = readSchema(connection);
      string error;
      assert(!ensureInventoryDatabaseSchema(connection, &error));
      assert(readUserVersion(connection) == beforeVersion);
      assert(readSchema(connection) == beforeSchema);
    }
    assert(readBytes(invalidPath) == migratedBytes);
    assert(migratedBytes != beforeBytes);
    {
      SqliteConnection connection;
      assert(openDatabaseReadOnly(invalidPath, connection));
      SqliteStatement statement;
      assert(sqliteApi().prepare_v2(connection.db, "SELECT id FROM inventatory_items", -1, &statement.stmt, nullptr) ==
             SQLITE_OK);
      assert(sqliteApi().step(statement.stmt) == SQLITE_ROW);
      assert(sqliteText(statement.stmt, 0) == "preserved-row");
    }
  }
  {
    SqliteConnection connection;
    assert(openDatabaseReadOnly(invalidPath, connection));
    assert(readUserVersion(connection) == 0);
    string error;
    assert(!validateInventoryDatabase(connection, &error));
  }

  {
    const auto completionPath = filesystem::temp_directory_path() / "inventatory-device-event-completion-test.db";
    filesystem::remove(completionPath, cleanupError);
    InventoryStore original;
    InventoryItem item;
    item.id = "completion-item";
    item.partName = "Completion part";
    item.quantity = 1;
    original.items().push_back(item);
    assert(original.save(completionPath));

    DeviceSyncRequest request;
    request.protocolVersion = 1;
    request.requestId = "completion-sync";
    request.deviceId = "device-a";
    request.events = {{"completion-event", "inventory.adjust", "completion-item", 1}};
    DeviceSyncResponse response;
    string error;
    assert(acceptDeviceSyncEvents(completionPath, request, response, error));
    assert(loadPendingDeviceSyncEvents(completionPath).size() == 1);

    vector<InventoryCommit> commits;
    assert(loadInventoryCommits(completionPath, commits));
    const auto initialCommitCount = commits.size();

    DeviceSyncResult mismatched;
    mismatched.resultId = "completion-result-wrong-device";
    mismatched.eventId = "completion-event";
    mismatched.deviceId = "device-b";
    mismatched.status = "completed";
    mismatched.requestedDelta = 1;
    mismatched.appliedDelta = 1;
    mismatched.quantity = 2;
    InventoryStore candidate = original;
    candidate.items().front().quantity = 2;
    assert(!completeDeviceSyncEvent(candidate, completionPath, mismatched, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount);
    assert(loadPendingDeviceSyncEvents(completionPath).size() == 1);

    DeviceSyncResult valid = mismatched;
    valid.resultId = "completion-result";
    valid.deviceId = "device-a";
    assert(completeDeviceSyncEvent(candidate, completionPath, valid, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount + 1);

    // A completed event cannot be applied a second time, even if the result
    // carries the correct device identity.  The failed update must roll back
    // the snapshot rewrite and must not append another inventory commit.
    assert(!completeDeviceSyncEvent(candidate, completionPath, valid, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount + 1);

    DeviceSyncResult nonexistent = valid;
    nonexistent.eventId = "no-such-event";
    nonexistent.resultId = "no-such-result";
    assert(!completeDeviceSyncEvent(candidate, completionPath, nonexistent, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount + 1);
    filesystem::remove(completionPath, cleanupError);
  }

  filesystem::remove(legacyPath, cleanupError);
  filesystem::remove(invalidPath, cleanupError);
#endif
}

int main() {
  {
    const auto first = advanceWorkspaceGeneration(0);
    const auto second = advanceWorkspaceGeneration(first);
    assert(first != 0);
    assert(second != first);
    assert(workspaceGenerationMatches(first, first));
    assert(!workspaceGenerationMatches(second, first));
    assert(!workspaceGenerationMatches(0, first));
    assert(advanceWorkspaceGeneration(numeric_limits<WorkspaceGeneration>::max()) == 1);
  }
  assert(onboardingRequired(false, false, 0));
  assert(onboardingRequired(false, true, 0));
  assert(!onboardingRequired(false, true, 1));
  assert(!onboardingRequired(true, false, 0));

  {
    const string key = "release-readiness-credential-test-" +
                       to_string(static_cast<unsigned long long>(chrono::steady_clock::now().time_since_epoch().count()));
    const bool initiallyErased = CredentialStore::erase(key);
    const bool wrote = CredentialStore::write(key, "temporary-secret-value");
    const auto stored = CredentialStore::read(key);
    const bool erased = CredentialStore::write(key, "");
    const bool missing = !CredentialStore::read(key).has_value();
    // Always clean the unique test target before asserting so a failed test
    // cannot leave a credential behind in the user's Credential Manager.
    CredentialStore::erase(key);
    assert(initiallyErased);
    assert(wrote);
    assert(stored.has_value() && *stored == "temporary-secret-value");
    assert(erased);
    assert(missing);
  }

#ifdef _WIN32
  // The persistence tests below must exercise the statically linked, pinned
  // SQLite amalgamation rather than an ambient sqlite3.dll.
  assert(sqliteApi().load());
  assert(sqlite3_libversion_number() == 3053004);
#endif

  // Keep the unit-aware search tests on the executable path. These used to be
  // declared and defined but never called, which allowed parser regressions
  // to pass the test suite unnoticed.
  testPhysicalValueParsing();
  testPhysicalValueMatching();
  testPhysicalValueSearchIntegration();
  testInventoryCommitHistory();
  testSqliteSchemaMigrationAndValidation();

  {
    assert(_putenv_s("INVENTATORY_TEST_ENVIRONMENT", "test-value") == 0);
    const auto value = environmentValue("INVENTATORY_TEST_ENVIRONMENT");
    assert(value.has_value());
    assert(*value == "test-value");
    assert(_putenv_s("INVENTATORY_TEST_ENVIRONMENT", "") == 0);
    assert(!environmentValue("INVENTATORY_TEST_ENVIRONMENT").has_value());
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
    InventoryItem outOfStock;
    outOfStock.id = "out";
    outOfStock.quantity = 0;
    InventoryItem atThreshold;
    atThreshold.id = "at";
    atThreshold.quantity = 3;
    InventoryItem aboveThreshold;
    aboveThreshold.id = "above";
    aboveThreshold.quantity = 4;
    const vector<InventoryItem> items = {outOfStock, atThreshold, aboveThreshold};

    assert(!isLowStock(outOfStock, 3));
    assert(isLowStock(atThreshold, 3));
    assert(!isLowStock(aboveThreshold, 3));
    assert(matchesQuery(atThreshold, "status:low", 3));
    assert(!matchesQuery(outOfStock, "status:low", 3));
    assert(filterItems(items, "status:low", 3).size() == 1);

    const auto summary = summarize(items, 3);
    assert(summary.lowStockCount == 1);
    const auto history = makeInventoryHistoryPoint(items, 3, 1710000000);
    assert(history.lowStockCount == 1);
    assert(history.outOfStockCount == 1);

    InventoryItem customThreshold;
    customThreshold.id = "custom-threshold";
    customThreshold.quantity = 6;
    customThreshold.reorderThreshold = 10;
    assert(effectiveReorderThreshold(customThreshold, 3) == 10);
    assert(isLowStock(customThreshold, 3));
    customThreshold.quantity = 11;
    assert(!isLowStock(customThreshold, 3));
    customThreshold.reorderThreshold = 0;
    customThreshold.quantity = 3;
    assert(effectiveReorderThreshold(customThreshold, 3) == 3);
    assert(isLowStock(customThreshold, 3));
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
      InventoryStore normalizedStore;
      InventatoryRack normalizedRack;
      normalizedRack.id = "normalized-rack";
      normalizedRack.code = "R9";
      normalizedRack.componentType = "integrated-circuits";
      normalizedStore.racks().push_back(normalizedRack);
      InventoryItem normalizedIc;
      normalizedIc.id = "normalized-ic";
      normalizedIc.partName = "Buck regulator";
      normalizedIc.category = "Integrated Circuits";
      normalizedIc.parameters = {{"Package / Case", "QFN-16"}};
      normalizedStore.items().push_back(normalizedIc);
      reconcileRackAssignment(normalizedStore, normalizedStore.items().back());
      assert(normalizedStore.racks().size() == 1);
      assert(rackLocation(normalizedStore.items().back(), normalizedStore.racks()) == "R9-A1");
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
    item.labelOverride = "Bench part";
    item.vendorMetadata.provider = "digikey";
    item.vendorMetadata.providerProductNumber = "123-ND";
    item.vendorMetadata.manufacturerPartNumber = "SKU-1";
    item.vendorMetadata.categoryId = "capacitors";
    item.vendorMetadata.categoryPath = {"Passive Components", "Capacitors"};
    item.vendorMetadata.title = "CAP CER 1UF";
    item.vendorMetadata.detailedDescription = "Ceramic capacitor";
    item.vendorMetadata.parameters = {{"Capacitance", "1uF"}};
    item.vendorMetadata.productUrl = "https://example.com/vendor-product";
    item.vendorMetadata.locale = "en";
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
    assert(restored.labelOverride == item.labelOverride);
    assert(restored.vendorMetadata.provider == item.vendorMetadata.provider);
    assert(restored.vendorMetadata.categoryPath == item.vendorMetadata.categoryPath);
    assert(restored.vendorMetadata.parameters.size() == 1);
    assert(restored.vendorMetadata.parameters.front().name == "Capacitance");
    assert(restored.vendorMetadata.parameters.front().value == "1uF");
  }

#ifdef _WIN32
  {
    // A failed rack read must not replace an existing in-memory store or make
    // the successfully read item subset eligible for a later write-back.
    const auto databasePath = filesystem::temp_directory_path() / "inventatory-incomplete-load-test.db";
    error_code removeError;
    filesystem::remove(databasePath, removeError);

    InventoryStore persisted;
    persisted.items().push_back({"persisted-item", "Persisted item", "Acme", "Resistors", 7});
    assert(persisted.save(databasePath));

    {
      SqliteConnection connection;
      assert(openDatabase(databasePath, connection));
      assert(execSql(connection, "DROP TABLE inventatory_racks; CREATE TABLE inventatory_racks (id TEXT PRIMARY KEY)"));
    }

    InventoryStore retained;
    retained.items().push_back({"live-item", "Live item", "Acme", "Capacitors", 3});
    assert(!retained.load(databasePath));
    assert(retained.items().size() == 1);
    assert(retained.items().front().id == "live-item");

    {
      SqliteConnection verification;
      assert(openDatabase(databasePath, verification));
      SqliteStatement statement;
      assert(sqliteApi().prepare_v2(verification.db, "SELECT COUNT(*) FROM inventatory_items", -1, &statement.stmt,
                                    nullptr) == SQLITE_OK);
      assert(sqliteApi().step(statement.stmt) == SQLITE_ROW);
      assert(sqliteApi().column_int(statement.stmt, 0) == 1);
    }
    filesystem::remove(databasePath, removeError);
    assert(!removeError);
  }
#endif

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
    item.labelOverride = "Bench diode";
    item.vendorMetadata.provider = "digikey";
    item.vendorMetadata.categoryPath = {"Diodes"};
    item.vendorMetadata.title = "General purpose diode";
    item.vendorMetadata.parameters = {{"Reverse Voltage", "40V"}};
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
    assert(loaded.items().front().labelOverride == "Bench diode");
    assert(loaded.items().front().vendorMetadata.provider == "digikey");
    assert(loaded.items().front().vendorMetadata.categoryPath == vector<string>({"Diodes"}));
    assert(loaded.items().front().vendorMetadata.parameters.size() == 1);
    filesystem::remove(tempPath);
  }

  {
    InventoryStore before;
    InventoryItem existing;
    existing.id = "movement-existing";
    existing.partName = "Existing movement item";
    existing.quantity = 5;
    before.items().push_back(existing);
    InventoryItem removed;
    removed.id = "movement-removed";
    removed.partName = "Removed movement item";
    removed.quantity = 4;
    before.items().push_back(removed);

    InventoryStore after = before;
    after.items().front().partName = "Renamed movement item";
    after.items().front().quantity = 8;
    after.items().erase(after.items().begin() + 1);
    InventoryItem added;
    added.id = "movement-added";
    added.partName = "Added movement item";
    added.quantity = 2;
    after.items().push_back(added);

    const auto movements = inventoryMovementDiff(before, after, "import", "order-42", 1710000200);
    assert(movements.size() == 3);
    const auto findMovement = [&](const string& id) -> const InventoryMovement* {
      for (const auto& movement : movements) {
        if (movement.itemId == id) return &movement;
      }
      return nullptr;
    };
    const auto* changed = findMovement("movement-existing");
    assert(changed != nullptr);
    assert(changed->itemName == "Renamed movement item");
    assert(changed->quantityBefore == 5);
    assert(changed->delta == 3);
    assert(changed->quantityAfter == 8);
    assert(changed->source == "import");
    assert(changed->reference == "order-42");
    const auto* addedMovement = findMovement("movement-added");
    assert(addedMovement != nullptr);
    assert(addedMovement->quantityBefore == 0);
    assert(addedMovement->delta == 2);
    assert(addedMovement->quantityAfter == 2);
    const auto* removedMovement = findMovement("movement-removed");
    assert(removedMovement != nullptr);
    assert(removedMovement->quantityBefore == 4);
    assert(removedMovement->delta == -4);
    assert(removedMovement->quantityAfter == 0);

    InventoryStore metadataOnly = after;
    metadataOnly.items().front().notes = "metadata changed";
    assert(inventoryMovementDiff(after, metadataOnly, "manual").empty());

#ifdef _WIN32
    const auto databasePath = filesystem::temp_directory_path() / "inventatory-stock-movements-test.db";
    error_code cleanupError;
    filesystem::remove(databasePath, cleanupError);
    assert(before.save(databasePath));
    assert(after.saveWithMovements(databasePath, movements));
    const auto loadedMovements = loadInventoryMovements(databasePath, 10);
    assert(loadedMovements.size() == 3);
    assert(loadedMovements.front().occurredAt == 1710000200);
    assert(loadInventoryMovements(databasePath, 2).size() == 2);
    filesystem::remove(databasePath, cleanupError);
#endif
  }

  {
    const string csv =
        "Indeks,Nr kat. DigiKey,Manufacturer Part Number,Producent,Opis,Numer referencyjny klienta,IloĹ›Ä‡,"
        "Niezrealizowana pozycja zamĂłwienia,Cena jednostkowa,WartoĹ›Ä‡\n"
        "1,308-1571-1-ND,CDMC6D28NP-4R7MC,Sumida America Components Inc.,FIXED IND 4.7UH 3.7A 46.4 MOHM,,10,0,"
        "\"2,62800 zĹ‚\",\"26,28 zĹ‚\"\n";

    const auto result = parseDigiKeyCsvText(csv, {});
    assert(result.ok);
    assert(result.candidates.size() == 1);
    const auto& candidate = result.candidates.front();
    assert(candidate.item.digikeyPartNumber == "308-1571-1-ND");
    assert(candidate.item.sku == "CDMC6D28NP-4R7MC");
    assert(candidate.item.manufacturer == "Sumida America Components Inc.");
    assert(candidate.item.quantity == 10);
    assert(candidate.item.category == "Inductors");
    assert(candidate.item.reorderThreshold == 0);
    assert(candidate.item.parameters.size() == 4);
  }

  {
    const string csv =
        "Index,Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity,Unit Price,Extended Price\n"
        "1,399-C0603C105K4RACTUCT-ND,C0603C105K4RACTU,KEMET,CAP CER 1UF 16V X7R 0603,50,\"0,13420 zĹ‚\",\"6,71 zĹ‚\"\n";

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
    const string csv =
        "Index,Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity\n"
        "1,123-ABC-ND,ABC-123,Acme,Test resistor,3\n"
        "2,123-ABC-ND,ABC-123,Acme,Test resistor,5\n";
    const auto result = parseDigiKeyCsvText(csv, {});
    assert(result.ok);
    assert(result.candidates.size() == 1);
    assert(result.candidates.front().item.quantity == 8);
    assert(result.candidates.front().warnings.size() == 1);
  }

  {
    const string csv =
        "Manufacturer Part Number,Manufacturer,Description,Quantity\n"
        "ABC-456,Acme,Fallback part,2\n"
        "ABC-456,Acme,Fallback part,4\n";
    const auto result = parseDigiKeyCsvText(csv, {});
    assert(result.ok);
    assert(result.candidates.size() == 1);
    assert(result.candidates.front().item.quantity == 6);
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
      assert(service.buildZpl(candidate).find("^FD" + expected + "^FS") != string::npos);
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
         {{"Output Voltage", "3.3V"}, {"Type", "LDO"}}, {}, "Linear Regulator"},
        {"golden-charger", "Li-ion battery charger", "Microchip", "Power Management ICs",
         {{"Function", "Battery Charger"}}, {}, "Battery Charger"},
        {"golden-load-switch", "Power distribution load switch", "Texas Instruments", "Power Management ICs",
         {{"Function", "Load Switch"}}, {}, "Load Switch"},
        {"golden-supervisor", "Voltage supervisor reset IC", "onsemi", "Power Management ICs",
         {{"Type", "Voltage Supervisor"}}, {}, "Volt Supervisor"},
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
         {{"Memory Type", "EEPROM"}, {"Memory Size", "256Kbit"}, {"Memory Interface", "I2C"}}, {}, "EEPROM"},
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
         {{"Core Processor", "ARM Cortex-M0+"}, {"Program Memory Size", "128KB"}}, {}, "Microcontroller"},
        {"golden-mosfet", "N-channel MOSFET", "Alpha & Omega", "MOSFETs",
         {{"FET Type", "N-Channel"}, {"Drain-Source Voltage", "30V"}, {"Rds On", "12mOhm"}}, {}, "N-MOSFET"},
        {"golden-bjt", "NPN transistor", "onsemi", "Transistors",
         {{"Transistor Type", "NPN"}, {"Collector Current", "600mA"}}, {}, "BJT NPN"},
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

    const auto vendorFixture = [&](const string& provider, const string& category, const string& title,
                                   vector<Parameter> parameters, const string& purpose, const string& print,
                                   PartLabelSource source) {
      InventoryItem candidate;
      candidate.vendorMetadata.provider = provider;
      candidate.vendorMetadata.categoryPath = {category};
      candidate.vendorMetadata.title = title;
      candidate.vendorMetadata.parameters = move(parameters);
      const auto descriptor = describePart(candidate);
      if (descriptor.purposeLabel != purpose || descriptor.printLabel != print || descriptor.source != source) {
        cerr << "Descriptor mismatch for category '" << category << "': expected '" << purpose << "' / '" << print
             << "', got '" << descriptor.purposeLabel << "' / '" << descriptor.printLabel << "'\n";
      }
      assert(descriptor.purposeLabel == purpose);
      assert(descriptor.printLabel == print);
      assert(descriptor.source == source);
    };

    // Each future adapter supplies the same metadata contract, so category
    // meaning is verified without any live supplier credentials.
    vendorFixture("digikey", "Fuses", "Fuse with ADC monitoring", {{"Resolution", "12 bit"}},
                  "Fuse", "Fuse", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Circuit Protection - Fuses", "FUSE GLASS 1A 250VAC", {},
                  "Fuse", "Fuse", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Inductors, Coils, Chokes - Fixed Inductors", "FIXED IND 4.7UH", {},
                  "Fixed Inductor", "Fixed Inductor", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Diodes - Rectifiers - Single", "DIODE SCHOTTKY 40V", {},
                  "Schottky Diode", "Schottky Diode", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Data Acquisition", "12-bit analog-to-digital converter", {{"Resolution", "12 bit"}},
                  "Analog-to-Digital Converter", "ADC", PartLabelSource::VendorRule);
    vendorFixture("tme", "Memory", "EEPROM timing controller", {{"Function", "Timer"}},
                  "EEPROM", "EEPROM", PartLabelSource::VendorRule);
    vendorFixture("digikey", "Clock/Timing", "Single timer oscillator", {},
                  "Timer IC", "Timer IC", PartLabelSource::VendorRule);
    vendorFixture("mouser", "Voltage Regulators", "LDO regulator", {{"Type", "LDO"}},
                  "Linear Voltage Regulator", "Linear Regulator", PartLabelSource::VendorRule);
    vendorFixture("tme", "Connectors", "Header", {{"Number of Positions", "8"}},
                  "Connector", "Connector", PartLabelSource::VendorCategory);

    // DigiKey discrete leaves are frequently represented by a broad path plus
    // a terminal taxonomy segment and structured electrical type.  The label
    // must preserve that concrete subtype rather than collapsing to a generic
    // semiconductor family.
    vendorFixture("digikey", "Transistors - Bipolar (BJT) - Single", "TRANS NPN 40V 0.2A SOT-23",
                  {{"Transistor Type", "NPN"}}, "NPN BJT Transistor", "BJT NPN", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Transistors - FETs, MOSFETs - Single", "MOSFET P-CH 30V 4A SOT-23",
                  {{"FET Type", "P-Channel"}}, "P-Channel MOSFET", "P-MOSFET", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Diodes - Zener - Single", "DIODE ZENER 5.1V 500MW SOD-123",
                  {{"Diode Type", "Zener"}}, "Zener Diode", "Zener Diode", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Diodes - TVS - Single", "TVS DIODE 24VWM 38.9VC SMA",
                  {{"Type", "Zener"}}, "TVS Diode", "TVS Diode", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Discrete Semiconductor Products", "DIODE SCHOTTKY 40V 3A DO214AC",
                  {{"Technology", "Schottky"}}, "Schottky Diode", "Schottky Diode", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Discrete Semiconductor Products", "TRANS PNP 40V 0.6A SOT23-3", {},
                  "PNP BJT Transistor", "BJT PNP", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Thyristors - TRIACs", "TRIAC 600V 4A TO-220", {},
                  "TRIAC", "TRIAC", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Capacitors - Ceramic Capacitors", "CAP CER 1UF 16V X7R 0603", {},
                  "Ceramic Capacitor", "Ceramic Cap", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Switches - Tactile Switches", "SWITCH TACTILE SPST-NO 0.05A 12V", {},
                  "Tactile Switch", "Tactile Switch", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Connectors, Interconnects - USB, DVI, HDMI Connectors", "CONN USB TYPE-C", {},
                  "USB Connector", "USB Connector", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Power Supplies - Board Mount - AC DC Converters", "AC/DC CONVERTER 5V 5W", {},
                  "AC-DC Converter", "AC-DC Converter", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Integrated Circuits (ICs)", "IC REG BUCK 3.3V 2A TSOT23-6",
                  {{"Topology", "Buck"}}, "Buck Converter", "Buck Converter", PartLabelSource::VendorRule);
    vendorFixture("digikey", "Integrated Circuits (ICs)", "IC EEPROM 32KBIT I2C 8SOIC",
                  {{"Memory Type", "EEPROM"}}, "EEPROM", "EEPROM", PartLabelSource::VendorRule);
    vendorFixture("digikey", "Integrated Circuits (ICs)", "IC USB TO UART BRIDGE SSOP-28",
                  {{"Function", "USB to UART"}}, "USB-UART Bridge", "USB-UART Bridge", PartLabelSource::VendorRule);
    vendorFixture("digikey", "Logic", "IC 8-BIT SHIFT REGISTER SOIC-16",
                  {{"Function", "Shift Register"}}, "Shift Register", "Shift Register", PartLabelSource::VendorRule);
    vendorFixture("digikey", "Clock/Timing", "IC RTC I2C 8SOIC",
                  {{"Function", "Real Time Clock"}}, "Real-Time Clock", "RTC", PartLabelSource::VendorRule);

    // Broad vendor-taxonomy coverage: categories, rather than loose words in
    // the product title, decide the concrete printed type.
    vendorFixture("digikey", "Switches - Tactile Switches", "TACTILE SWITCH", {},
                  "Tactile Switch", "Tactile Switch", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Pushbutton Switches", "Illuminated pushbutton", {},
                  "Pushbutton Switch", "Pushbutton", PartLabelSource::VendorCategory);
    vendorFixture("tme", "Diodes - Zener Diodes", "Zener diode 5.1V", {},
                  "Zener Diode", "Zener Diode", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Circuit Protection - Varistors, MOVs", "MOV 275VAC", {},
                  "Varistor", "Varistor", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Resistors - Chip Resistor - Surface Mount", "RES 10K", {},
                  "Surface-Mount Resistor", "SMD Resistor", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Capacitors - Ceramic Capacitors", "CAP CER 1UF", {},
                  "Ceramic Capacitor", "Ceramic Cap", PartLabelSource::VendorCategory);
    vendorFixture("tme", "Inductors, Coils, Chokes - Ferrite Beads and Chips", "FERRITE BEAD", {},
                  "Ferrite Bead", "Ferrite Bead", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Connectors, Interconnects - USB, DVI, HDMI Connectors", "USB TYPE-C", {},
                  "USB Connector", "USB Connector", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Optocouplers", "Phototransistor output", {},
                  "Optocoupler", "Optocoupler", PartLabelSource::VendorCategory);
    vendorFixture("tme", "Relays - Signal Relays", "SIGNAL RELAY", {},
                  "Signal Relay", "Signal Relay", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "Sensors, Transducers - Temperature Sensors - Analog and Digital Output", "I2C SENSOR", {},
                  "Temperature Sensor", "Temp Sensor", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Power Supplies - Board Mount - DC DC Converters", "DC/DC MODULE", {},
                  "DC-DC Converter", "DC-DC Converter", PartLabelSource::VendorCategory);
    vendorFixture("tme", "PMIC - Voltage Regulators - DC DC Switching Regulators", "BUCK REGULATOR", {},
                  "Switching Regulator", "Switch Regulator", PartLabelSource::VendorCategory);
    vendorFixture("digikey", "RF and Wireless - RF Transceiver Modules and Modems", "WIRELESS MODULE", {},
                  "RF Module", "RF Module", PartLabelSource::VendorCategory);
    vendorFixture("mouser", "Development Boards, Kits, Programmers", "EVALUATION BOARD", {},
                  "Development Board", "Dev Board", PartLabelSource::VendorCategory);

    InventoryItem genericConverter;
    genericConverter.vendorMetadata.provider = "digikey";
    genericConverter.vendorMetadata.categoryPath = {"Data Acquisition"};
    genericConverter.vendorMetadata.parameters = {{"Resolution", "12 bit"}};
    assert(describePart(genericConverter).printLabel == "Data Converter");

    InventoryItem overrideItem;
    overrideItem.labelOverride = "Workshop custom fuse";
    const auto overridden = describePart(overrideItem);
    assert(overridden.purposeLabel == "Workshop custom fuse");
    assert(overridden.printLabel == "Workshop custom fuse");
    assert(overridden.source == PartLabelSource::ManualOverride);

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
    assert(zpl.find("^FO10,0^GB236,24,24,B,6^FS") != string::npos);
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
                           {"Current - Peak Pulse (10/1000Âµs)", "23.1A"},
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

    InventoryItem circuitProtectionTvs;
    circuitProtectionTvs.partName = "SURGE SUPPRESSOR 24V";
    circuitProtectionTvs.category = "Circuit Protection";
    circuitProtectionTvs.parameters = {{"Technology", "TVS"}};
    assert(service.buildLabelPlan(circuitProtectionTvs).categoryHeader == "TVS Diode");

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
      const auto hostileTimerPlan = expectHeader(hostileTimer, "Memory IC");
      assert(hostileTimerPlan.categoryHeader != "Timer IC");
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
      const auto falsePositivePlan = expectHeader(buckFalsePositive, "Memory IC");
      assert(service.buildZpl(buckFalsePositive).find("Rectifier Diode") == string::npos);
      assert(service.buildZpl(buckFalsePositive).find("^FDMemory IC^FS") != string::npos);
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
      assert(genericPlan.categoryHeader == "IC");
      assert(genericPlan.categoryHeader != "Memory IC");
    }

    InventoryItem fallback;
    fallback.id = "misc-1";
    fallback.partName = "Prototype module";
    fallback.manufacturer = "Acme";
    fallback.category = "Misc / Prototype";
    const auto fallbackPlan = service.buildLabelPlan(fallback);
    assert(fallbackPlan.categoryHeader == "Prototype");
    assert(service.buildZpl(fallback).find("^FDPrototype^FS") != string::npos);

    InventoryItem unclassified;
    unclassified.partName = "Uncatalogued item";
    assert(describePart(unclassified).printLabel == "Unclassified Component");
    assert(describePart(unclassified).printLabel != "Part");

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
    item.sku = "IRLML6344TRPBF";
    item.inventatoryId = "Inventatory:T-00012";
    item.parameters = {
        {"Package / Case", "TO-263-3, D2PAK (2 Leads + Tab)"},
        {"Drain-Source Voltage", "30V"},
        {"Continuous Drain Current", "12A"},
        {"Power - Max", "2W (Ta)"},
    };

    const auto plan = service.buildLabelPlan(item);
    assert(plan.categoryHeader == "N-MOSFET");
    assert(plan.mainValue == "IRLML6344TRPBF");
    assert(plan.packageLine == "TO-263-3");
    assert(plan.parameterLine1.find("Vds") != string::npos);
    assert(plan.parameterLine1.find("30V") != string::npos);
    assert(plan.parameterLine2.find("Id") != string::npos);
    assert(plan.parameterLine2.find("12A") != string::npos);
    assert(plan.parameterLine3.empty());

    const auto zpl = service.buildZpl(item);
    assert(zpl.find("^FDN-MOSFET^FS") != string::npos);
    assert(zpl.find("^FDIRLML6344TRPBF^FS") != string::npos);
    assert(zpl.find("2W (Ta)") == string::npos);
    assert(zpl.find("TO-263-3,") == string::npos);
    assert(zpl.find("^FO10,33^A0N,34,31^FDIRLML6344TRPBF^FS") != string::npos);
    assert(zpl.find("^FO10,70^A0N,14,14^FDTO-263-3^FS") != string::npos);
    assert(zpl.find("^FDVds 30V^FS") != string::npos);
    assert(zpl.find("^FDId 12A^FS") != string::npos);
  }

  {
    LabelPrinterService service(make_unique<MockPrinterBackend>());
    InventoryItem longHeader;
    longHeader.partName = "Part number retained as the main value";
    longHeader.labelOverride = "Custom label beyond sixteen";
    const auto zpl = service.buildZpl(longHeader);
    assert(partShortDescription(longHeader) == "Custom label beyond sixteen");
    assert(zpl.find("^FDCustom label beyond sixteen^FS") != string::npos);
    assert(zpl.find("^FO10,0^GB236,24,24,B,6^FS") != string::npos);
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
    const InventatoryScanConfig expected{"r1-test", string(64, 'a'), "192.168.1.2", 8080, true};
    assert(saveInventatoryScanConfig(configPath, expected));
    InventatoryScanConfig loaded;
    assert(loadInventatoryScanConfig(configPath, loaded));
    assert(loaded.deviceId == expected.deviceId);
    assert(loaded.token.empty());
    assert(loaded.fallbackHost == expected.fallbackHost);
    assert(loaded.fallbackPort == expected.fallbackPort);
    assert(loaded.setupComplete);
    assert(loaded.paired());

    const auto pendingPath = filesystem::temp_directory_path() / "inventatory-scan-config-pending-test.conf";
    const InventatoryScanConfig pending{"", string(64, 'b'), "", 0, true};
    assert(saveInventatoryScanConfig(pendingPath, pending));
    InventatoryScanConfig pendingLoaded;
    assert(loadInventatoryScanConfig(pendingPath, pendingLoaded));
    assert(pendingLoaded.setupComplete);
    assert(pendingLoaded.paired());
    filesystem::remove(pendingPath);

    const auto legacyPath = filesystem::temp_directory_path() / "inventatory-scan-config-legacy-test.conf";
    {
      ofstream legacy(legacyPath, ios::trunc);
      legacy << "device_id=r1-legacy\n"
             << "fallback_host=192.168.1.3\n"
             << "fallback_port=8081\n";
    }
    InventatoryScanConfig legacyLoaded;
    assert(loadInventatoryScanConfig(legacyPath, legacyLoaded));
    assert(legacyLoaded.setupComplete);
    filesystem::remove(legacyPath);

    {
      ofstream truncated(configPath, ios::trunc);
      truncated << "device_id=r1-test\n";
    }
    assert(!loadInventatoryScanConfig(configPath, loaded));
    filesystem::remove(configPath);
    assert(generateInventatoryScanToken().size() == 64);
    assert(deviceRequestMac("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "POST",
                            "/api/v1/device/sync", "r1-test", 42, R"({"protocolVersion":1})") ==
           "d99b3246f156ffc7b1cf9507f98aeb47603e93f103945badc3e95641e5c8f685");
    assert(deviceResponseMac("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 42, 200,
                             R"({"ok":true})") ==
           "ec5e858f7b38260c65df43b570e13a7dceab39361d4f17ebf3fe8d4523024423");
  }

  {
    const auto stateDirectory = filesystem::temp_directory_path() / "inventatory-http-test";
    error_code cleanupError;
    filesystem::remove_all(stateDirectory, cleanupError);
    const auto replayState = stateDirectory / "replay.state";
    const string token = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const string deviceId = "r1-secure";
    const string body =
        R"({"protocolVersion":1,"requestId":"secure-sync","deviceId":"r1-secure","firmwareVersion":"0.1.0","mode":"ready","rssi":-40,"queueDepth":0,"events":[],"resultAcks":[]})";
    atomic<int> syncCalls{0};
    auto onSync = [&syncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response, string&) {
      ++syncCalls;
      response.requestId = request.requestId;
      return true;
    };

    LocalHttpServer server;
    server.setDeviceCredentials(deviceId, token, replayState);
    assert(server.start(19430, onSync));
    const auto firstRequest = signedSyncRequest(token, deviceId, 42, body);
    const auto accepted = sendLocalHttpRequest(server.port(), firstRequest);
    assert(accepted.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(accepted.find("X-Inventatory-Mac:") != string::npos);
    assert(accepted.find(token) == string::npos);
    assert(syncCalls == 1);

    auto modifiedRequest = firstRequest;
    const auto firmwareVersion = modifiedRequest.find("0.1.0");
    assert(firmwareVersion != string::npos);
    modifiedRequest.replace(firmwareVersion, 5, "9.9.9");
    const auto modified = sendLocalHttpRequest(server.port(), modifiedRequest);
    assert(modified.rfind("HTTP/1.1 401 Unauthorized", 0) == 0);
    assert(syncCalls == 1);

    const auto replayed = sendLocalHttpRequest(server.port(), firstRequest);
    assert(replayed.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 1);

    const auto slowClient = connectSlowLocalClient(server.port());
    const auto before = chrono::steady_clock::now();
    const auto nextResponse = sendLocalHttpRequest(server.port(), signedSyncRequest(token, deviceId, 43, body));
    const auto elapsed = chrono::steady_clock::now() - before;
    closesocket(slowClient);
    assert(nextResponse.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(elapsed < chrono::seconds(1));
    assert(syncCalls == 2);

    const string rotatedToken = "111102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    server.setDeviceCredentials(deviceId, rotatedToken, replayState);
    const auto oldTokenAfterRotation = sendLocalHttpRequest(server.port(), signedSyncRequest(token, deviceId, 44, body));
    assert(oldTokenAfterRotation.rfind("HTTP/1.1 401 Unauthorized", 0) == 0);
    const auto newTokenAfterRotation = sendLocalHttpRequest(server.port(), signedSyncRequest(rotatedToken, deviceId, 44, body));
    assert(newTokenAfterRotation.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(syncCalls == 3);
    server.stop();

    LocalHttpServer restarted;
    restarted.setDeviceCredentials(deviceId, rotatedToken, replayState);
    assert(restarted.start(19450, onSync));
    const auto persistedReplay = sendLocalHttpRequest(restarted.port(), signedSyncRequest(rotatedToken, deviceId, 44, body));
    assert(persistedReplay.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 3);
    restarted.stop();

    const auto concurrentReplayState = stateDirectory / "concurrent-replay.state";
    atomic<int> concurrentSyncCalls{0};
    atomic<bool> duplicateRequestStarted{false};
    string duplicateResponse;
    uint16_t concurrentPort = 0;
    auto concurrentOnSync = [&](const DeviceSyncRequest& request, DeviceSyncResponse& response, string&) {
      ++concurrentSyncCalls;
      if (!duplicateRequestStarted.exchange(true)) {
        duplicateResponse = sendLocalHttpRequest(
            concurrentPort, signedSyncRequest(token, deviceId, 100, body), 1000);
      }
      response.requestId = request.requestId;
      return true;
    };
    LocalHttpServer concurrentServer;
    concurrentServer.setDeviceCredentials(deviceId, token, concurrentReplayState);
    assert(concurrentServer.start(19460, concurrentOnSync));
    concurrentPort = concurrentServer.port();
    const auto concurrentAccepted =
        sendLocalHttpRequest(concurrentServer.port(), signedSyncRequest(token, deviceId, 100, body));
    assert(concurrentAccepted.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(duplicateResponse.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(concurrentSyncCalls == 1);
    concurrentServer.stop();

    const auto retryReplayState = stateDirectory / "retry-replay.state";
    atomic<int> retrySyncCalls{0};
    auto retryOnSync = [&retrySyncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response,
                                         string& error) {
      if (++retrySyncCalls == 1) {
        error = "durable callback unavailable";
        return false;
      }
      response.requestId = request.requestId;
      return true;
    };
    LocalHttpServer retryServer;
    retryServer.setDeviceCredentials(deviceId, token, retryReplayState);
    assert(retryServer.start(19480, retryOnSync));
    const auto failedSync =
        sendLocalHttpRequest(retryServer.port(), signedSyncRequest(token, deviceId, 101, body));
    assert(failedSync.rfind("HTTP/1.1 503 Service Unavailable", 0) == 0);
    const auto retriedSync =
        sendLocalHttpRequest(retryServer.port(), signedSyncRequest(token, deviceId, 101, body));
    assert(retriedSync.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(retrySyncCalls == 2);
    retryServer.stop();

    const auto corruptReplayState = stateDirectory / "corrupt-replay.state";
    {
      ofstream corrupt(corruptReplayState, ios::trunc);
      corrupt << "fingerprint=" << deviceTransportStateFingerprint(rotatedToken) << '\n'
              << "counter=not-a-counter\n";
    }
    LocalHttpServer corruptStateServer;
    corruptStateServer.setDeviceCredentials(deviceId, rotatedToken, corruptReplayState);
    assert(corruptStateServer.start(19470, onSync));
    const auto rejectedWithCorruptState =
        sendLocalHttpRequest(corruptStateServer.port(), signedSyncRequest(rotatedToken, deviceId, 1, body));
    assert(rejectedWithCorruptState.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 3);
    corruptStateServer.stop();

    filesystem::remove_all(stateDirectory, cleanupError);
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
        R"({"protocolVersion":1,"requestId":"sync-1","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":1,"capabilities":["lcd.128x64"],"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":["r1-a-76-result"]})",
        request, error));
    assert(request.protocolVersion == kInventatoryScanTransportProtocolVersion);
    assert(request.events.size() == 1);
    assert(request.events.front().value == 2);
    assert(request.resultAcks.size() == 1);
    assert(!request.hasLookup);
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-lookup","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-9","code":"0002"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.lookupId == "lookup-9");
    assert(request.lookup.code == "0002");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-digikey-lookup","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-dk-1","code":"718-2362-1-ND"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.code == "718-2362-1-ND");
    // A component Data Matrix carries the manufacturer part number, spaces and all.
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-mpn-lookup","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"lookup-mpn-1","code":"C1F 500"}})",
        request, error));
    assert(request.hasLookup);
    assert(request.lookup.code == "C1F 500");
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-label","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"label_print","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"quickLabelPrint":{"requestId":"r1-a-label-1","presetIndex":2,"revision":3}})",
        request, error));
    assert(request.hasQuickLabelPrint);
    assert(request.quickLabelPrint.presetIndex == 2);
    assert(request.quickLabelPrint.revision == 3);
    // An unresolvable lookup code is dropped so the events it travels with are
    // still delivered; only a structurally broken lookup rejects the envelope.
    assert(parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-odd-lookup","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[{"eventId":"r1-a-7","type":"inventory.receive","code":"C1F 500","value":5}],"resultAcks":[],"lookup":{"lookupId":"lookup-10","code":"AB*C"}})",
        request, error));
    assert(!request.hasLookup);
    assert(request.events.size() == 1);
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-bad-lookup","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"await_quantity","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"lookup":{"lookupId":"","code":"0002"}})",
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
    // The baseline accepts only protocol v1 envelopes.
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":99,"requestId":"sync-2","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[]})",
        request, error));
    assert(error == "Unsupported protocol version");

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
        R"({"protocolVersion":1,"requestId":"sync-3","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":"r1-a-77","type":"inventory.adjust","code":"0002","value":2}],"resultAcks":[]})",
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

    // A newly received device event is auto-printed immediately after its event is
    // committed. Its live item must receive the same identifiers as the SQLite
    // snapshot; otherwise the label has blank Inventatory text and an empty QR field.
    InventoryItem autoLabelItem;
    autoLabelItem.id = "auto-label-new-item";
    autoLabelItem.partName = "Auto label IC";
    autoLabelItem.category = "Integrated Circuits";
    autoLabelItem.lastUpdated = time(nullptr);
    autoLabelItem.createdAt = autoLabelItem.lastUpdated;
    candidate.items().push_back(autoLabelItem);
    assert(completeDeviceSyncEvent(candidate, databasePath, result, &store));
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
    assert(response.results.front().deviceId == "r1-a");
    assert(response.results.front().quantity == 7);
    InventoryStore reloaded;
    assert(reloaded.load(databasePath));
    assert(reloaded.findByMachineCode("0002")->quantity == 7);
    vector<InventoryCommit> eventCommits;
    assert(loadInventoryCommits(databasePath, eventCommits));
    assert(eventCommits.size() == 2);
    assert(eventCommits.front().source == "scanner");
    assert(eventCommits.front().reference == result.eventId);
    assert(eventCommits.front().parentId == eventCommits.back().id);
    const auto eventMovements = loadInventoryMovements(databasePath);
    bool foundEventMovement = false;
    for (const auto& movement : eventMovements) {
      if (movement.itemId == "sync-item" && movement.reference == result.eventId && movement.delta == 2) {
        foundEventMovement = true;
        break;
      }
    }
    assert(foundEventMovement);

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
    expected.latestAvailableVersion = "0.1.1";
    expected.latestReleaseUrl = "https://example.invalid/Inventatory/releases/tag/v0.1.1";
    expected.deviceServicePort = 8181;
    expected.digiKeyClientId = "client-id";
    expected.digiKeyAccountId = "account-id";
    expected.digiKeySite = "PL";
    expected.digiKeyLanguage = "pl";
    expected.digiKeyCurrency = "PLN";
    expected.lowStockThreshold = 12;
    expected.appearance.colors[static_cast<size_t>(AppearanceColorRole::CanvasBg)] = 0x123456;
    expected.appearance.colors[static_cast<size_t>(AppearanceColorRole::Interactive)] = 0xABCDEF;
    expected.appearance.colors[static_cast<size_t>(AppearanceColorRole::DangerBg)] = 0x000000;
    assert(saveAppSettings(path, expected));

    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.schemaVersion == 1);
    assert(loaded.completedOnboardingVersion == 1);
    assert(loaded.dataDirectory == expected.dataDirectory);
    assert(loaded.printerQueue == expected.printerQueue);
    assert(!loaded.autoPrintScannedLabels);
    assert(loaded.backgroundServiceEnabled);
    assert(loaded.backgroundConsentAsked);
    assert(!loaded.updateChecksEnabled);
    assert(loaded.lastUpdateCheckUnixSeconds == 123456789);
    assert(loaded.latestAvailableVersion == "0.1.1");
    assert(loaded.latestReleaseUrl == expected.latestReleaseUrl);
    assert(loaded.deviceServicePort == 8181);
    assert(loaded.digiKeyClientId == "client-id");
    assert(loaded.digiKeyAccountId == "account-id");
    assert(loaded.digiKeySite == "PL");
    assert(loaded.digiKeyLanguage == "pl");
    assert(loaded.digiKeyCurrency == "PLN");
    assert(loaded.lowStockThreshold == 12);
    assert(loaded.appearance.colors[static_cast<size_t>(AppearanceColorRole::CanvasBg)] == 0x123456);
    assert(loaded.appearance.colors[static_cast<size_t>(AppearanceColorRole::Interactive)] == 0xABCDEF);
    assert(loaded.appearance.colors[static_cast<size_t>(AppearanceColorRole::DangerBg)] == 0x000000);

    uint32_t parsedColor = 0;
    assert(parseAppearanceColorHex(" #aBcDeF ", parsedColor));
    assert(parsedColor == 0xABCDEF);
    assert(appearanceColorHex(parsedColor) == "#ABCDEF");
    assert(parseAppearanceColorHex("000000", parsedColor));
    assert(parsedColor == 0x000000);
    assert(parseAppearanceColorHex("#FFFFFF", parsedColor));
    assert(parsedColor == 0xFFFFFF);
    assert(!parseAppearanceColorHex("#12345", parsedColor));
    assert(!parseAppearanceColorHex("#12345G", parsedColor));

    applyUiAppearance(expected.appearance);
    assert(uiCanvasBg() == ftxui::Color::RGB(0x12, 0x34, 0x56));
    assert(uiAccentColor() == ftxui::Color::RGB(0xAB, 0xCD, 0xEF));
    assert(uiPanelLeftBg() == uiSurfaceBg());
    assert(uiRowSelectedBg() == uiSelectionBg());
    applyUiAppearance(AppearanceSettings{});

    ifstream persisted(path);
    const string text((istreambuf_iterator<char>(persisted)), istreambuf_iterator<char>());
    assert(text.find("client_secret") == string::npos);
    assert(text.find("secret") == string::npos);
    assert(text.find("device_service_port=8181") != string::npos);
    assert(text.find("low_stock_threshold=12") != string::npos);
    assert(text.find("appearance_canvas_bg=#123456") != string::npos);
    assert(text.find("appearance_interactive=#ABCDEF") != string::npos);
    assert(text.find("quick_label") == string::npos);
    assert(text.find("tolerance_") == string::npos);
    persisted.close();
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    auto activePaths = makeInventatoryDataPaths(filesystem::path("C:/Inventatory/current"));
    const auto originalPaths = activePaths;
    int saveAttempts = 0;
    assert(!switchInventatoryDataPathsAfterSaving(activePaths, filesystem::path("C:/Inventatory/new"), [&] {
      ++saveAttempts;
      return false;
    }));
    assert(saveAttempts == 1);
    assert(activePaths.dataDirectory == originalPaths.dataDirectory);
    assert(activePaths.inventory == originalPaths.inventory);
    assert(activePaths.printer == originalPaths.printer);
    assert(activePaths.activity == originalPaths.activity);
    assert(activePaths.scanConfig == originalPaths.scanConfig);

    assert(switchInventatoryDataPathsAfterSaving(activePaths, filesystem::path("C:/Inventatory/new"), [] {
      return true;
    }));
    assert(activePaths.dataDirectory == filesystem::path("C:/Inventatory/new"));
    assert(activePaths.inventory == activePaths.dataDirectory / "inventory.db");
    assert(activePaths.printer == activePaths.dataDirectory / "printer.conf");
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
    const auto executablePath = L"C:\\Program Files\\Inventatory\\inventatory.exe";
    const auto launcherPath = buildBackgroundStartupLauncherPath(executablePath);
    assert(launcherPath == L"C:\\Program Files\\Inventatory\\inventatory-background.exe");
    assert(buildBackgroundStartupCommand(launcherPath) ==
           L"\"C:\\Program Files\\Inventatory\\inventatory-background.exe\" --background");
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-unsupported-settings-test.conf";
    ofstream unsupported(path, ios::trunc);
    unsupported << "schema_version=0\n";
    unsupported.close();
    AppSettings loaded;
    assert(!loadAppSettings(path, loaded));
    ifstream preserved(path);
    const string preservedText((istreambuf_iterator<char>(preserved)), istreambuf_iterator<char>());
    assert(preservedText.find("schema_version=0") != string::npos);
    preserved.close();
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-legacy-settings-test.conf";
    ofstream legacy(path, ios::trunc);
    legacy << "schema_version=1\n";
    legacy << "data_directory=\"legacy\"\n";
    legacy << "appearance_canvas_bg=#1234G7\n";
    legacy << "tolerance_capacitance=0.02\n";
    legacy.close();
    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.lowStockThreshold == kDefaultLowStockThreshold);
    assert(loaded.appearance.colors[static_cast<size_t>(AppearanceColorRole::CanvasBg)] == 0x0D1010);
    assert(loaded.appearance.colors[static_cast<size_t>(AppearanceColorRole::DangerFlashBg)] == 0x70403B);
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-invalid-threshold-settings-test.conf";
    ofstream invalid(path, ios::trunc);
    invalid << "schema_version=1\n";
    invalid << "low_stock_threshold=0\n";
    invalid.close();
    AppSettings loaded;
    assert(loadAppSettings(path, loaded));
    assert(loaded.lowStockThreshold == kDefaultLowStockThreshold);
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    assert(isVersionNewer("v0.1.1", "0.1.0"));
    assert(isVersionNewer("0.1.1", "0.1.0"));
    assert(!isVersionNewer("0.1.0", "0.1.0"));
    assert(!isVersionNewer("preview", "0.1.0"));
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

    const auto inferred = parseKicadBomText("Designator,Designation\nR1 R2,10k\n", "inferred");
    assert(inferred.ok);
    assert(inferred.lines.front().quantityPerBoard == 2);
    const auto malformed = parseKicadBomText("Designator,Designation,Quantity\nR1 R2,10k,two\n", "malformed");
    assert(!malformed.ok);
    assert(!malformed.warnings.empty());
    const auto explicitQuantity = parseKicadBomText("Designator,Designation,Quantity\nR1 R2,10k,7\n", "explicit");
    assert(explicitQuantity.ok);
    assert(explicitQuantity.lines.front().quantityPerBoard == 7);
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
    assert(!bomBuildReady(analysis));

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
    assert(bomBuildReady(analysis) == (analysis.shortCount == 0 && !analysis.matches.empty()));
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

  {
    const auto source = filesystem::temp_directory_path() / "inventatory-transfer-source";
    const auto backup = filesystem::temp_directory_path() / "inventatory-transfer-backup";
    const auto bundle = filesystem::temp_directory_path() / "inventatory-transfer-bundle";
    const auto restoreTarget = filesystem::temp_directory_path() / "inventatory-transfer-restore-target";
    const auto settingsPath = filesystem::temp_directory_path() / "inventatory-transfer-settings.conf";
    const auto targetSettingsPath = filesystem::temp_directory_path() / "inventatory-transfer-target-settings.conf";
    const auto csv = filesystem::temp_directory_path() / "inventatory-transfer-export.csv";
    error_code cleanupError;
    filesystem::remove_all(source, cleanupError);
    filesystem::remove_all(backup, cleanupError);
    filesystem::remove_all(bundle, cleanupError);
    filesystem::remove_all(restoreTarget, cleanupError);
    filesystem::remove(settingsPath, cleanupError);
    filesystem::remove(targetSettingsPath, cleanupError);
    filesystem::remove(csv, cleanupError);
    filesystem::create_directories(source);

    InventoryStore store;
    InventoryItem item;
    item.id = "transfer-item";
    item.partName = "Transfer resistor";
    item.manufacturer = "Inventatory Test";
    item.category = "Resistors";
    item.quantity = 12;
    item.location = "Drawer 1";
    item.lastUpdated = 1710000000;
    item.tags = {"test", "release"};
    item.parameters = {{"Resistance", "10k"}};
    store.items().push_back(item);
    ensureInventoryIdentifiers(store.items());
    reconcileRackAssignments(store);
    assert(store.save(source / "inventory.db"));
    assert(ensureInventoryCommitHistory(source / "inventory.db", store));
    InventoryStore changedStore = store;
    changedStore.items().front().quantity = 13;
    InventoryCommitDraft changedDraft;
    changedDraft.source = "manual";
    changedDraft.message = "Backup history fixture";
    assert(changedStore.saveWithCommit(source / "inventory.db", store, changedDraft));
    {
      ofstream(source / "activity.tsv") << "test activity\n";
      ofstream(source / "quick_labels.conf") << "quick_label_revision=1\n";
    }

    string error;
    assert(exportInventoryCsv(store, csv, error));
    ifstream exported(csv);
    const string exportedText((istreambuf_iterator<char>(exported)), istreambuf_iterator<char>());
    assert(exportedText.find("Transfer resistor") != string::npos);
    assert(exportedText.find("Quantity") != string::npos);
    assert(backupInventatoryData(source, backup, error));
    assert(filesystem::exists(backup / "inventory.db"));
    vector<InventoryCommit> backupCommits;
    assert(loadInventoryCommits(backup / "inventory.db", backupCommits));
    assert(backupCommits.size() == 2);
    assert(filesystem::exists(backup / "activity.tsv"));
    assert(filesystem::exists(backup / "quick_labels.conf"));

    AppSettings backupSettings;
    backupSettings.dataDirectory = source;
    backupSettings.completedOnboardingVersion = 1;
    assert(saveAppSettings(settingsPath, backupSettings));
    assert(createInventatoryBackup(source, settingsPath, bundle, "1.0.0", error));
    assert(filesystem::exists(bundle / "manifest.tsv"));
    assert(filesystem::exists(bundle / "inventory.db"));
    vector<InventoryCommit> bundleCommits;
    assert(loadInventoryCommits(bundle / "inventory.db", bundleCommits));
    assert(bundleCommits.size() == 2);
    assert(!filesystem::exists(bundle / "inventatory_scan.conf"));
    assert(validateInventatoryBackup(bundle, error));
    filesystem::create_directories(restoreTarget);
    InventoryStore protectedStore;
    InventoryItem protectedItem;
    protectedItem.id = "protected-restore-item";
    protectedItem.partName = "Protected current item";
    protectedStore.items().push_back(protectedItem);
    assert(protectedStore.save(restoreTarget / "inventory.db"));
    AppSettings targetSettings;
    targetSettings.dataDirectory = restoreTarget;
    assert(saveAppSettings(targetSettingsPath, targetSettings));
    {
      ofstream corrupt(bundle / "activity.tsv", ios::app);
      corrupt << "corrupt\n";
    }
    assert(!validateInventatoryBackup(bundle, error));
    assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error));
    InventoryStore unchangedStore;
    assert(unchangedStore.load(restoreTarget / "inventory.db"));
    assert(unchangedStore.items().front().id == "protected-restore-item");
    filesystem::remove_all(bundle, cleanupError);
    assert(createInventatoryBackup(source, settingsPath, bundle, "1.0.0", error));
    assert(validateInventatoryBackup(bundle, error));
    assert(restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error));
    InventoryStore restoredStore;
    assert(restoredStore.load(restoreTarget / "inventory.db"));
    assert(restoredStore.items().size() == 1);
    vector<InventoryCommit> restoredCommits;
    assert(loadInventoryCommits(restoreTarget / "inventory.db", restoredCommits));
    assert(restoredCommits.size() == 2);
    AppSettings restoredSettings;
    assert(loadAppSettings(targetSettingsPath, restoredSettings));
    assert(restoredSettings.dataDirectory == restoreTarget);

    filesystem::remove_all(source, cleanupError);
    filesystem::remove_all(backup, cleanupError);
    filesystem::remove_all(bundle, cleanupError);
    filesystem::remove_all(restoreTarget, cleanupError);
    filesystem::remove(settingsPath, cleanupError);
    filesystem::remove(targetSettingsPath, cleanupError);
    filesystem::remove(csv, cleanupError);
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-device-event-recovery-test.db";
    error_code cleanupError;
    filesystem::remove(path, cleanupError);
    InventoryStore store;
    InventoryItem item;
    item.id = "event-recovery-item";
    item.machineCode = "0007";
    item.partName = "Recovery part";
    item.quantity = 1;
    store.items().push_back(item);
    assert(store.save(path));

    DeviceSyncRequest request;
    request.protocolVersion = 1;
    request.requestId = "recovery-sync";
    request.deviceId = "r1-recovery";
    request.firmwareVersion = "0.1.0";
    request.mode = "ready";
    request.rssi = -40;
    request.queueDepth = 1;
    request.events = {{"recovery-event", "inventory.adjust", "9999", 1}};
    DeviceSyncResponse response;
    string error;
    assert(acceptDeviceSyncEvents(path, request, response, error));
    const auto pending = loadPendingDeviceSyncEvents(path);
    assert(pending.size() == 1);

    DeviceSyncResult failed;
    failed.resultId = "recovery-event-result";
    failed.eventId = "recovery-event";
    failed.status = "failed";
    failed.code = "unknown_item";
    failed.message = "Unknown machine code";
    assert(completeDeviceSyncEvent(store, path, failed));
    const auto failedRecords = loadDeviceSyncEventRecords(path);
    assert(failedRecords.size() == 1);
    assert(failedRecords.front().resultStatus == "failed");

    size_t retried = 0;
    assert(retryFailedDeviceSyncEvents(path, retried));
    assert(retried == 1);
    assert(loadPendingDeviceSyncEvents(path).size() == 1);
    assert(completeDeviceSyncEvent(store, path, failed));
    size_t discarded = 0;
    assert(discardFailedDeviceSyncEvents(path, discarded));
    assert(discarded == 1);
    assert(loadDeviceSyncEventRecords(path).empty());
    filesystem::remove(path, cleanupError);
  }

  cout << "Inventatory core tests passed\n";
  return 0;
}
