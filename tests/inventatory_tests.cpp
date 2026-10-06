#include "core/inventory/Inventory.h"
#include "App.h"
#include "app/UpdateWizardPresentation.h"
#include "app/shell/AppBootstrap.h"
#include "app/settings/AppSettings.h"
#include "app/common/AppActionSupport.h"
#include "platform/system/UpdateService.h"
#include "platform/system/Console.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/Environment.h"
#include "platform/digikey/DigiKeyApi.h"
#include "core/inventory/InventoryInternals.h"
#include "core/inventory/InventoryMerge.h"
#include "core/storage/AtomicFile.h"
#include "core/storage/InventorySqlite.h"
#include "core/transfer/InventoryTransfer.h"
#include "core/transfer/CsvExport.h"
#ifdef near
#undef near
#endif
#include "core/scanner/InventatoryScanProtocol.h"
#include "platform/scanner/HttpServer.h"
#include "platform/security/CredentialStore.h"
#ifdef near
#undef near
#endif
#include "core/parts/PartDescriptor.h"
#include "import/digikey/DigiKeyCsvImport.h"
#include "import/csv/CsvFormat.h"
#include "import/csv/CsvReader.h"
#include "import/kicad/KicadBom.h"
#include "core/bom/BomMatch.h"
#include "platform/digikey/DigiKeyApiPrivate.h"
#include "core/bom/BomProjectStore.h"
#include "label_printer/core/LabelPrinter.h"
#include "label_printer/core/LabelPrinterPrivate.h"
#include "label_printer/platform/CupsStatus.h"
#include "app/shell/AppNavigation.h"
#include "ui/pages/history/HistoryPagePrivate.h"
#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/pages/settings/SettingsPagePrivate.h"
#include "ui/shared/AppUiShared.h"
#include "ui/pages/stock/StockFilterState.h"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#include <atomic>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <stdlib.h>
#else
#include <ws2tcpip.h>
#endif

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
using inventatory::label_printer_detail::estimateFont0Width;
using inventatory::label_printer_detail::fitFont0Text;
using inventatory::label_printer_detail::labelTileValue;

namespace {

#ifdef _WIN32
constexpr const char* kTestApplicationArchive = "Inventatory-win-x64.zip";
constexpr const char* kTestApplicationChecksums = "SHA256SUMS.txt";
constexpr const char* kTestApplicationInstaller = "Install-Inventatory.ps1";
#else
constexpr const char* kTestApplicationArchive = "Inventatory-linux-x64.tar.gz";
constexpr const char* kTestApplicationChecksums = "SHA256SUMS-linux.txt";
constexpr const char* kTestApplicationInstaller = "Install-Inventatory.sh";
#endif

string currentPlatformReleaseFixture(string fixture) {
#ifndef _WIN32
  const auto replace = [&fixture](const string& before, const string& after) {
    size_t position = 0;
    while ((position = fixture.find(before, position)) != string::npos) {
      fixture.replace(position, before.size(), after);
      position += after.size();
    }
  };
  replace("Inventatory-win-x64.zip", kTestApplicationArchive);
  replace("SHA256SUMS.txt", kTestApplicationChecksums);
  replace("Install-Inventatory.ps1", kTestApplicationInstaller);
#endif
  return fixture;
}

#ifndef _WIN32
class ScopedEnvironment final {
 public:
  ScopedEnvironment(const char* name, const optional<string>& value) : name_(name) {
    const char* previous = getenv(name);
    if (previous != nullptr) previous_ = string(previous);
    if (value.has_value()) setenv(name, value->c_str(), 1);
    else unsetenv(name);
  }
  ~ScopedEnvironment() {
    if (previous_.has_value()) setenv(name_.c_str(), previous_->c_str(), 1);
    else unsetenv(name_.c_str());
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  string name_;
  optional<string> previous_;
};

string readTextFile(const filesystem::path& path) {
  ifstream stream(path, ios::binary);
  return string((istreambuf_iterator<char>(stream)), istreambuf_iterator<char>());
}

void writeTextFile(const filesystem::path& path, const string& text, bool executable = false) {
  filesystem::create_directories(path.parent_path());
  {
    ofstream stream(path, ios::binary | ios::trunc);
    stream << text;
  }
  filesystem::permissions(path,
                          executable ? filesystem::perms::owner_all
                                     : (filesystem::perms::owner_read | filesystem::perms::owner_write),
                          filesystem::perm_options::replace);
}

bool processIsGone(pid_t process) {
  if (kill(process, 0) != 0) return errno == ESRCH;
  // An orphan that nobody reaps stays visible as a zombie; it is dead all the same.
  ifstream stat("/proc/" + to_string(process) + "/stat");
  string line;
  getline(stat, line);
  const auto paren = line.rfind(')');
  return paren != string::npos && paren + 2U < line.size() && line[paren + 2U] == 'Z';
}
#endif

time_t localTime(int year, int month, int day, int hour = 12, int minute = 0) {
  tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_isdst = -1;
  return mktime(&value);
}

void testHistoryPagePresentationData() {
  using history_page_detail::HistoryCommitGroup;
  using history_page_detail::HistoryFieldDiff;
  using history_page_detail::HistoryRecord;
  using history_page_detail::HistorySourceFilter;

  InventoryCommit manual;
  manual.id = "manual-id";
  manual.sequence = 24;
  manual.timestamp = localTime(2026, 9, 16, 19, 27);
  manual.source = "manual";
  manual.message = "Updated inventory";
  manual.changedItemCount = 1;

  InventoryCommit corrective;
  corrective.id = "corrective-id";
  corrective.sequence = 23;
  corrective.timestamp = localTime(2026, 9, 16, 19, 20);
  corrective.source = "revert";
  corrective.message = "Restored snapshot";
  corrective.corrective = true;
  corrective.changedItemCount = 1;

  InventoryCommit digiKey;
  digiKey.id = "digikey-id";
  digiKey.sequence = 16;
  digiKey.timestamp = localTime(2026, 9, 4, 23, 47);
  digiKey.source = "digikey";
  digiKey.message = "DigiKey refresh batch";
  digiKey.changedItemCount = 94;

  InventoryCommit checkpoint;
  checkpoint.id = "checkpoint-id";
  checkpoint.sequence = 15;
  checkpoint.timestamp = localTime(2026, 9, 4, 18, 24);
  checkpoint.source = "checkpoint";
  checkpoint.message = "Before reorg";
  checkpoint.checkpoint = true;

  const vector<InventoryCommit> commits = {manual, corrective, digiKey, checkpoint};
  assert(history_page_detail::historyCommitType(manual) == "MANUAL");
  assert(history_page_detail::historyCommitType(corrective) == "CORRECTIVE");
  assert(history_page_detail::historyCommitType(digiKey) == "DIGIKEY");
  assert(history_page_detail::historyCommitType(checkpoint) == "CHECKPOINT");
  assert(history_page_detail::historyCommitImpactSummary(manual) == "1 part changed");
  assert(history_page_detail::historyCommitImpactSummary(digiKey) == "94 parts changed");
  assert(history_page_detail::historyCommitImpactSummary(checkpoint) == "No inventory changes");

  const auto correctiveOnly = history_page_detail::filteredHistoryIndices(commits, {}, HistorySourceFilter::Corrective);
  assert((correctiveOnly == vector<size_t>{1}));
  const auto searchResults = history_page_detail::filteredHistoryIndices(commits, "refresh", HistorySourceFilter::All);
  assert((searchResults == vector<size_t>{2}));
  const auto messageAndTypeResults =
      history_page_detail::filteredHistoryIndices(commits, "DIGI", HistorySourceFilter::All);
  assert((messageAndTypeResults == vector<size_t>{2}));

  const auto groups = history_page_detail::groupedHistoryCommits(commits, {}, HistorySourceFilter::All,
                                                                  localTime(2026, 9, 16, 20));
  assert(groups.size() == 2);
  assert(groups[0].label == "Today");
  assert((groups[0].indices == vector<size_t>{0, 1}));
  assert(groups[1].label == "Sep 04, 2026");
  assert((groups[1].indices == vector<size_t>{2, 3}));

  InventoryCommit legacyMessage;
  legacyMessage.message = "Updated inventory · 1 part, 0 racks";
  assert(history_page_detail::historyCommitDisplayMessage(legacyMessage) == "Updated inventory");

  HistoryRecord record;
  record.entityType = "item";
  record.entityId = "item-id";
  record.label = "RES-10K";
  record.changes = {
      {"item", "item-id", "RES-10K", "record", "", "modified"},
      {"item", "item-id", "RES-10K", "quantity", "8", "12"},
      {"item", "item-id", "RES-10K", "location", "R2-B3", "R2-B4"},
  };
  const auto diffs = history_page_detail::historyFieldDiffs(record);
  assert(diffs.size() == 2);
  assert((diffs[0].field == "Quantity" && diffs[0].previous == "8" && diffs[0].next == "12"));
  assert((diffs[1].field == "Location" && diffs[1].previous == "R2-B3" && diffs[1].next == "R2-B4"));

  HistoryRecord providerRecord;
  providerRecord.entityType = "item";
  providerRecord.entityId = "provider-item";
  providerRecord.label = "Provider item";
  providerRecord.changes.push_back({"item", "provider-item", "Provider item", "parameters",
                                    "Quantity Available=118283;Package=0603",
                                    "Quantity Available=117833;Package=0603"});
  providerRecord.changes.push_back({"item", "provider-item", "Provider item", "vendor parameters",
                                    "Stock=118283", "Stock=117833"});
  providerRecord.changes.push_back({"item", "provider-item", "Provider item", "quantity", "7", "12"});
  const auto providerDiffs = history_page_detail::historyFieldDiffs(providerRecord);
  assert(providerDiffs.size() == 1);
  assert(providerDiffs.front().field == "Quantity");
  assert(providerDiffs.front().previous == "7");
  assert(providerDiffs.front().next == "12");
}

#ifndef _WIN32
void testAtomicFileLinks() {
  const auto root = filesystem::temp_directory_path() / ("inventatory-atomic-file-test-" + to_string(getpid()));
  error_code ignored;
  filesystem::remove_all(root, ignored);
  filesystem::create_directories(root / "real");
  filesystem::create_directories(root / "config");
  string error;

  // Plain replacement leaves exactly one file behind.
  assert(writeFileAtomically(root / "config" / "plain.conf", "first", &error));
  assert(writeFileAtomically(root / "config" / "plain.conf", "second", &error));
  assert(readTextFile(root / "config" / "plain.conf") == "second");
  size_t entries = 0;
  for (const auto& entry : filesystem::directory_iterator(root / "config")) {
    (void)entry;
    ++entries;
  }
  assert(entries == 1);
  assert(writeFileAtomically(root / "config" / "nested" / "deeper.conf", "created", &error));

  // A symbolic link (a dotfile-managed settings file) keeps being a link; its target gets the content.
  writeTextFile(root / "real" / "settings.conf", "old");
  filesystem::create_symlink("../real/settings.conf", root / "config" / "settings.conf");
  assert(writeFileAtomically(root / "config" / "settings.conf", "new", &error));
  assert(filesystem::is_symlink(root / "config" / "settings.conf"));
  assert(filesystem::read_symlink(root / "config" / "settings.conf") == filesystem::path("../real/settings.conf"));
  assert(readTextFile(root / "real" / "settings.conf") == "new");
  entries = 0;
  for (const auto& entry : filesystem::directory_iterator(root / "real")) {
    (void)entry;
    ++entries;
  }
  assert(entries == 1);

  // A link whose target does not exist yet creates the target.
  filesystem::create_symlink((root / "real" / "later.conf").string(), root / "config" / "later.conf");
  assert(writeFileAtomically(root / "config" / "later.conf", "created through link", &error));
  assert(filesystem::is_symlink(root / "config" / "later.conf"));
  assert(readTextFile(root / "real" / "later.conf") == "created through link");

  // A cyclic chain cannot be followed; the write still succeeds and replaces the link itself.
  filesystem::create_symlink("loop-b.conf", root / "config" / "loop-a.conf");
  filesystem::create_symlink("loop-a.conf", root / "config" / "loop-b.conf");
  assert(writeFileAtomically(root / "config" / "loop-a.conf", "loop", &error));
  assert(readTextFile(root / "config" / "loop-a.conf") == "loop");

  // Flushing helpers succeed for what exists and report what does not.
  assert(syncFile(root / "real" / "settings.conf", &error));
  assert(syncDirectory(root / "real", &error));
  assert(!syncFile(root / "real" / "missing.conf", &error) && !error.empty());
  assert(!syncDirectory(root / "real" / "missing-directory", &error) && !error.empty());
  filesystem::remove_all(root, ignored);
}
#endif

void testHistoryPageLayoutData() {
  const auto wide = history_page_detail::historyPaneWidths(179);
  assert(wide[0] == 89);
  assert(wide[1] == 89);
  assert(wide[0] + wide[1] + 1 == 179);

  const auto minimum = history_page_detail::historyPaneWidths(98);
  assert(minimum[0] == 48);
  assert(minimum[1] == 49);
  assert(minimum[0] + minimum[1] + 1 == 98);
}

void testPrimaryNavigationContract() {
  const auto& entries = inventatory::app_navigation::primaryNavigationEntries();
  assert(entries.size() == 6);
  const std::array<std::string, 6> expectedIds = {"stock", "racks", "import", "projects", "history", "settings"};
  const std::array<std::string, 6> expectedLabels = {"Stock", "Racks", "Import", "Projects", "History", "Settings"};
  for (size_t index = 0; index < entries.size(); ++index) {
    assert(entries[index].shortcut == static_cast<char>('1' + index));
    assert(std::string(entries[index].id) == expectedIds[index]);
    assert(std::string(entries[index].label) == expectedLabels[index]);
    assert(static_cast<unsigned int>(entries[index].page) == index);
  }
  assert(inventatory::app_navigation::primaryNavigationEntryForShortcut('1') == &entries[0]);
  assert(inventatory::app_navigation::primaryNavigationEntryForShortcut('6') == &entries[5]);
  assert(inventatory::app_navigation::primaryNavigationEntryForShortcut('7') == nullptr);
}

#ifndef _WIN32
// The Linux publisher talks to avahi-daemon over D-Bus, but only for an address
// that belongs to an up, non-loopback private-LAN interface.  Anything else must
// be refused by the interface gate before any D-Bus traffic, so these cases are
// deterministic whether or not avahi-daemon runs on the test machine.
void testLinuxMdnsRefusesNonPrivateInterfaces() {
  MdnsService service;
  service.stop();  // stop() without a prior start() is a no-op
  assert(!service.running());
  assert(!service.start(4567, "127.0.0.1"));       // loopback is never advertised
  assert(!service.start(4567, "0.0.0.0"));         // wildcard is not a private LAN address
  assert(!service.start(4567, ""));                // no bound address
  assert(!service.start(4567, "not-an-address"));  // not parseable as an interface address
  assert(!service.start(4567, "203.0.113.77"));    // documentation range: public, never assigned
  assert(!service.running());
  service.stop();
  service.stop();  // stop() is idempotent
  assert(!service.running());
}
#endif

bool setScannerTestAddress(sockaddr_in& address) {
  const auto privateAddresses = privateLocalAddresses();
  const string host = privateAddresses.empty() ? "127.0.0.1" : privateAddresses.front();
#ifdef _WIN32
  return InetPtonA(AF_INET, host.c_str(), &address.sin_addr) == 1;
#else
  return inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1;
#endif
}

string sendLocalHttpRequest(uint16_t port, const string& request, int receiveTimeout = 3000) {
  NativeSocket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(client != kInvalidSocket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  assert(setScannerTestAddress(address));
  address.sin_port = htons(port);
  assert(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
  assert(send(client, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()));

#ifdef _WIN32
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
#else
  timeval timeout{};
  timeout.tv_sec = receiveTimeout / 1000;
  timeout.tv_usec = (receiveTimeout % 1000) * 1000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
  string response;
  array<char, 1024> buffer{};
  int received = 0;
  while ((received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0)) > 0) {
    response.append(buffer.data(), static_cast<size_t>(received));
  }
  closeSocket(client);
  return response;
}

NativeSocket connectSlowLocalClient(uint16_t port) {
  NativeSocket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(client != kInvalidSocket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  assert(setScannerTestAddress(address));
  address.sin_port = htons(port);
  assert(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
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

string labelTile(const InventatoryLabelPlan& plan, const string& caption) {
  for (const auto& tile : plan.parameters) {
    if (tile.caption == caption) return tile.value;
  }
  return {};
}

size_t zplOccurrences(const string& zpl, const string& needle) {
  size_t count = 0;
  for (auto at = zpl.find(needle); at != string::npos; at = zpl.find(needle, at + needle.size())) ++count;
  return count;
}

}  // namespace

// Forward declarations for physical value tests
void testPhysicalValueParsing();
void testPhysicalValueMatching();
void testPhysicalValueSearchIntegration();
void testPhysicalValueCommaDecimalLocale();
void testDecimalParsingCommaLocale();
void testStockFilterState();

void testStockFilterState() {
  assert(stockFilterMenuItemAt(0) == StockFilterMenuItem::Date);
  assert(stockFilterMenuItemAt(3) == StockFilterMenuItem::Za);
  assert(stockFilterMenuItemAt(4) == StockFilterMenuItem::Reset);
  assert(stockFilterMenuSelection(StockDateFilter::All, StockSortOrder::Az) == 2);
  assert(stockFilterMenuSelection(StockDateFilter::Last7Days, StockSortOrder::Za) == 0);

  auto dateFilter = StockDateFilter::Last30Days;
  auto sortOrder = StockSortOrder::Quantity;
  int selection = 0;
  bool dateSubmenuOpen = true;
  resetStockFilterState(dateFilter, sortOrder, selection, dateSubmenuOpen);
  assert(dateFilter == StockDateFilter::All);
  assert(sortOrder == StockSortOrder::Az);
  assert(selection == 2);
  assert(!dateSubmenuOpen);
}

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

  // RKM resistor notation keeps working.
  assert(parsePhysicalValue("4R7")->type == PhysicalValueType::Resistance);
  assert(std::abs(parsePhysicalValue("4R7")->value - 4.7) < 1e-12);
  assert(std::abs(parsePhysicalValue("R280")->value - 0.28) < 1e-12);
  assert(std::abs(parsePhysicalValue("280R")->value - 280.0) < 1e-9);
  assert(parsePhysicalValue("4K7")->type == PhysicalValueType::Resistance);
  assert(std::abs(parsePhysicalValue("4K7")->value - 4700.0) < 1e-9);
  assert(std::abs(parsePhysicalValue("4k7")->value - 4700.0) < 1e-9);
  assert(parsePhysicalValue("2M2")->type == PhysicalValueType::Resistance);
  assert(std::abs(parsePhysicalValue("2M2")->value - 2.2e6) < 1e-3);
  assert(std::abs(parsePhysicalValue("1G5")->value - 1.5e9) < 1.0);

  // Ambiguous p/n/u/m markers in RKM position are not resistances (4u7 can be
  // 4.7 uF or 4.7 uH), and a prefix marker without a head digit is not a value
  // (M3 is a screw size), so queries fall back to ordinary text matching.
  for (const char* text : {"4u7", "4U7", "4n7", "4p7", "4m7", "M3", "U1", "K4", "G1", "k7", "n5"}) {
    assert(!parsePhysicalValue(text).has_value());
  }

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

// Set INVENTATORY_REQUIRE_LOCALE_TESTS=1 (CI) to make a missing comma-decimal locale a failure
// instead of a silent skip, so the locale regression tests cannot quietly stop running.
void skipOrFailMissingLocale(const char* testName) {
  const char* require = getenv("INVENTATORY_REQUIRE_LOCALE_TESTS");
  if (require != nullptr && string(require) == "1") {
    cerr << "No comma-decimal locale installed but INVENTATORY_REQUIRE_LOCALE_TESTS=1: " << testName << '\n';
    std::exit(1);
  }
}

// The application calls setlocale(LC_ALL, ""), so value parsing and search must
// keep working when the user's locale writes decimals with a comma.
void testPhysicalValueCommaDecimalLocale() {
  const char* candidates[] = {"pl_PL.UTF-8", "pl_PL.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8"};
  const string previous = setlocale(LC_ALL, nullptr);
  bool commaLocale = false;
  for (const char* name : candidates) {
    if (setlocale(LC_ALL, name) != nullptr && localeconv()->decimal_point[0] == ',') {
      commaLocale = true;
      break;
    }
  }
  if (!commaLocale) {
    setlocale(LC_ALL, previous.c_str());
    skipOrFailMissingLocale("testPhysicalValueCommaDecimalLocale");
    cout << "No comma-decimal locale installed; skipping locale regression test\n";
    return;
  }

  const auto tenth = parsePhysicalValue("0.1uF");
  assert(tenth.has_value());
  assert(std::abs(tenth->value - 1e-7) < 1e-15);
  assert(parsePhysicalValue("4.7k Ohm").has_value());
  assert(std::abs(parsePhysicalValue("4R7")->value - 4.7) < 1e-12);
  assert(std::abs(parsePhysicalValue("1e-3F")->value - 1e-3) < 1e-15);
  assert(std::abs(parsePhysicalValue(".5uH")->value - 5e-7) < 1e-15);
  assert(comparePhysicalValues(string("0.1 ") + "\xC2\xB5" + "F", "100nf")->band == PhysicalValueMatchBand::Exact);
  assert(comparePhysicalValues("0.1UF", "100nf")->band == PhysicalValueMatchBand::Exact);
  assert(!parsePhysicalValue("0.1.2uF").has_value());

  InventoryItem parameterOnly;
  parameterOnly.id = "locale-parameter";
  parameterOnly.partName = "Ceramic capacitor";
  parameterOnly.quantity = 1;
  parameterOnly.parameters = {{"Capacitance", string("0.1 ") + "\xC2\xB5" + "F"}, {"Tolerance", "\xC2\xB1" "10%"}};
  InventoryItem nameOnly;
  nameOnly.id = "locale-name";
  nameOnly.partName = "CAP CER 0.1UF 16V X7R 0603";
  nameOnly.quantity = 1;
  InventoryItem unrelated;
  unrelated.id = "locale-other";
  unrelated.partName = "CAP CER 1UF 16V X7R 0603";
  unrelated.quantity = 1;
  const vector<InventoryItem> items = {parameterOnly, nameOnly, unrelated};
  for (const char* query : {"100nf", "100nF", "0.1uf", "100000pf"}) {
    const auto matches = filterItems(items, query);
    assert(matches.size() == 2);
    assert(items[matches[0]].id != "locale-other" && items[matches[1]].id != "locale-other");
  }
  assert(findClosestPhysicalValues(items, "100nF").size() >= 2);

  setlocale(LC_ALL, previous.c_str());
}

// BOM value parsing and DigiKey numeric-field validation read dot-decimal text,
// which must not depend on the user's locale.
void testDecimalParsingCommaLocale() {
  const char* candidates[] = {"pl_PL.UTF-8", "pl_PL.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8"};
  const string previous = setlocale(LC_ALL, nullptr);
  const auto checkAll = [] {
    ValueKind kind = ValueKind::Resistance;
    const auto tenth = parseElectricalValue("0.1uF", kind);
    assert(tenth.has_value() && kind == ValueKind::Capacitance && std::abs(*tenth - 1e-7) < 1e-15);
    const auto resistor = parseElectricalValue("4.7k", kind);
    assert(resistor.has_value() && std::abs(*resistor - 4700.0) < 1e-9);
    assert(parseElectricalValue("4k7", kind).has_value());
    assert(!parseElectricalValue("1.2.3k", kind).has_value());

    assert(digikey_detail::isFiniteDecimal("0.1", 1000.0));
    assert(digikey_detail::isFiniteDecimal("12.50", 1000.0));
    assert(digikey_detail::isFiniteDecimal("1e2", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("1000.5", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("0,1", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("0.1x", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("-1", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("nan", 1000.0));
    assert(!digikey_detail::isFiniteDecimal("", 1000.0));
  };

  checkAll();  // Baseline in the current locale.
  for (const char* name : candidates) {
    if (setlocale(LC_ALL, name) != nullptr && localeconv()->decimal_point[0] == ',') {
      checkAll();
      setlocale(LC_ALL, previous.c_str());
      return;
    }
  }
  setlocale(LC_ALL, previous.c_str());
  skipOrFailMissingLocale("testDecimalParsingCommaLocale");
  cout << "No comma-decimal locale installed; skipping decimal locale regression test\n";
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

  // Values that only appear in the part name must match equivalent spellings.
  {
    vector<InventoryItem> named;
    InventoryItem nameOnly;
    nameOnly.id = "name-only-01uf";
    nameOnly.partName = "0.1uF";
    nameOnly.quantity = 10;
    named.push_back(nameOnly);
    InventoryItem spaced;
    spaced.id = "name-only-spaced";
    spaced.partName = "Ceramic 0.1 uF X7R";
    spaced.quantity = 10;
    named.push_back(spaced);
    InventoryItem other;
    other.id = "name-only-1uf";
    other.partName = "1uF";
    other.quantity = 10;
    named.push_back(other);
    InventoryItem resistor;
    resistor.id = "name-only-100ohm";
    resistor.partName = "100 Ohm";
    resistor.quantity = 10;
    named.push_back(resistor);

    const auto byName = filterItems(named, "100nf");
    assert(byName.size() == 2);
    assert(named[byName[0]].id != "name-only-1uf" && named[byName[1]].id != "name-only-1uf");
    assert(filterItems(named, "100nF").size() == 2);
    assert(!findClosestPhysicalValues(named, "100nF").empty());
  }

  // Capacitor RKM notation and screw sizes are not read as resistances, so
  // they match as ordinary text and do not rank resistors.
  {
    vector<InventoryItem> notation;
    InventoryItem screw;
    screw.id = "notation-screw";
    screw.partName = "M3x8 screw";
    screw.quantity = 10;
    notation.push_back(screw);
    InventoryItem standoff;
    standoff.id = "notation-standoff";
    standoff.partName = "M3 Standoff";
    standoff.quantity = 10;
    notation.push_back(standoff);
    InventoryItem resistor300k;
    resistor300k.id = "notation-300k";
    resistor300k.partName = "Resistor";
    resistor300k.quantity = 10;
    resistor300k.parameters = {{"Resistance", "300 kOhm"}};
    notation.push_back(resistor300k);
    InventoryItem capacitor;
    capacitor.id = "notation-4u7";
    capacitor.partName = "Capacitor 4u7 16V";
    capacitor.quantity = 10;
    notation.push_back(capacitor);

    const auto screws = filterItems(notation, "M3");
    assert(screws.size() == 2);
    for (const size_t index : screws) {
      assert(notation[index].id == "notation-screw" || notation[index].id == "notation-standoff");
    }
    for (const auto& match : rankedFilterItems(notation, "M3", {}, 5)) {
      assert(!match.hasPhysicalComparison);
    }

    const auto caps = filterItems(notation, "4u7");
    assert(caps.size() == 1);
    assert(notation[caps[0]].id == "notation-4u7");
  }

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
#ifdef INVENTATORY_SQLITE_STORAGE
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

  // Commit snapshots retain the pre-v1 structured encoding and must reject
  // truncated or trailing records instead of silently dropping metadata.
  {
    const auto serialized = serializeItem(item);
    InventoryItem restored;
    assert(deserializeItemStrict(serialized, restored));
    assert(restored.parameters.size() == item.parameters.size());
    assert(restored.parameters[0].name == item.parameters[0].name);
    assert(restored.parameters[0].value == item.parameters[0].value);
    assert(restored.vendorMetadata.categoryPath == item.vendorMetadata.categoryPath);
    assert(!deserializeItemStrict(serialized + " trailing", restored));
    auto legacySerialized = serialized;
    size_t prefix = 0;
    while ((prefix = legacySerialized.find("v1:", prefix)) != string::npos) {
      legacySerialized.replace(prefix, 3, "v2:");
      prefix += 3;
    }
    assert(deserializeItemStrict(legacySerialized, restored));
    assert(restored.parameters.size() == item.parameters.size());
    assert(restored.parameters[0].name == item.parameters[0].name);
    assert(restored.parameters[0].value == item.parameters[0].value);
  }

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

void testSqliteSchemaValidation() {
#ifdef INVENTATORY_SQLITE_STORAGE
  const auto unsupportedPath = filesystem::temp_directory_path() / "inventatory-unsupported-schema-test.db";
  const auto invalidPath = filesystem::temp_directory_path() / "inventatory-invalid-schema-test.db";
  error_code cleanupError;
  filesystem::remove(unsupportedPath, cleanupError);
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
    assert(openDatabase(unsupportedPath, connection));
    assert(execSql(connection, R"SQL(
      CREATE TABLE inventatory_items (
        id TEXT PRIMARY KEY, part_name TEXT NOT NULL, manufacturer TEXT NOT NULL, category TEXT NOT NULL,
        quantity INTEGER NOT NULL, reorder_threshold INTEGER NOT NULL, location TEXT NOT NULL,
        tags TEXT NOT NULL, parameters TEXT NOT NULL, notes TEXT NOT NULL,
        manufacturer_part_number TEXT NOT NULL, datasheet_url TEXT NOT NULL, enrichment_status TEXT NOT NULL,
        last_updated INTEGER NOT NULL
      );
    )SQL"));
  }
  {
    const auto beforeBytes = readBytes(unsupportedPath);
    SqliteConnection connection;
    assert(openDatabase(unsupportedPath, connection));
    const auto beforeVersion = readUserVersion(connection);
    const auto beforeSchema = readSchema(connection);
    string error;
    assert(!ensureInventoryDatabaseSchema(connection, &error));
    assert(readUserVersion(connection) == beforeVersion);
    assert(readSchema(connection) == beforeSchema);
    assert(!validateInventoryDatabase(connection, &error));
    assert(readBytes(unsupportedPath) == beforeBytes);
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
    string invalidBytes;
    {
      SqliteConnection connection;
      assert(openDatabase(invalidPath, connection));
      assert(execSql(connection, "CREATE TABLE inventatory_items (id TEXT PRIMARY KEY)"));
      assert(execSql(connection, "INSERT INTO inventatory_items (id) VALUES ('preserved-row')"));
      invalidBytes = readBytes(invalidPath);
      const auto beforeVersion = readUserVersion(connection);
      const auto beforeSchema = readSchema(connection);
      string error;
      assert(!ensureInventoryDatabaseSchema(connection, &error));
      assert(readUserVersion(connection) == beforeVersion);
      assert(readSchema(connection) == beforeSchema);
    }
    assert(readBytes(invalidPath) == invalidBytes);
    assert(invalidBytes != beforeBytes);
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
    const auto pendingCompletion = loadPendingDeviceSyncEvents(completionPath);
    assert(pendingCompletion.size() == 1);
    assert(pendingCompletion.front().deviceId == "device-a");

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
    // The first versioned write records both the original snapshot and the
    // scanner mutation in the same transaction.
    assert(commits.size() == initialCommitCount + 2);

    // A completed event cannot be applied a second time, even if the result
    // carries the correct device identity.  The failed update must roll back
    // the snapshot rewrite and must not append another inventory commit.
    assert(!completeDeviceSyncEvent(candidate, completionPath, valid, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount + 2);

    DeviceSyncResult nonexistent = valid;
    nonexistent.eventId = "no-such-event";
    nonexistent.resultId = "no-such-result";
    assert(!completeDeviceSyncEvent(candidate, completionPath, nonexistent, &original));
    assert(loadInventoryCommits(completionPath, commits));
    assert(commits.size() == initialCommitCount + 2);
    filesystem::remove(completionPath, cleanupError);
  }

  filesystem::remove(unsupportedPath, cleanupError);
  filesystem::remove(invalidPath, cleanupError);
#endif
}

void testPackageGHardening() {
  {
    DigiKeyConfig baseline;
    baseline.clientId = "client-id";
    baseline.clientSecret = "client-secret";
    baseline.accountId = "account-id";
    baseline.site = "US";
    baseline.language = "en";
    baseline.currency = "USD";
    assert(baseline.valid());

    const vector<string DigiKeyConfig::*> headerFields = {
        &DigiKeyConfig::clientId, &DigiKeyConfig::accountId, &DigiKeyConfig::site,
        &DigiKeyConfig::language, &DigiKeyConfig::currency,
    };
    const vector<string> invalidValues = {"value\r\nnext", string("value") + '\t' + "next",
                                          string("value") + '\x01' + "next", string("value") + '\x7f' + "next"};
    for (const auto field : headerFields) {
      for (const auto& value : invalidValues) {
        auto invalid = baseline;
        invalid.*field = value;
        assert(!invalid.valid());
      }
    }
    auto oversized = baseline;
    oversized.clientId.assign(4097, 'x');
    assert(!oversized.valid());
    oversized = baseline;
    oversized.clientSecret.assign(4097, 'x');
    assert(!oversized.valid());
  }

  {
    string error;
    assert(validateDigiKeyJsonPayload(R"({"ok":true,"value":-0.25e+2,"text":"\\u20ac"})", &error));
    assert(error.empty());

    const vector<string> malformed = {
        R"({"bad":"\q"})",
        R"({"bad":"\u12g4"})",
        R"({"bad":"\uD800"})",
        R"({"bad":"\uDC00"})",
        R"({"a":1,"a":2})",
        R"({"number":01})",
        R"({"number":1.})",
        R"({"number":1e})",
        R"({"number":-})",
    };
    for (const auto& payload : malformed) {
      error.clear();
      assert(!validateDigiKeyJsonPayload(payload, &error));
      assert(!error.empty());
    }

    string deeplyNested = "0";
    for (size_t index = 0; index < 66; ++index) deeplyNested = "{\"nested\":" + deeplyNested + "}";
    assert(!validateDigiKeyJsonPayload(deeplyNested, &error));
    assert(error.find("nesting") != string::npos);

    const string oversizedNumber = "{\"number\":" + string(4097, '1') + "}";
    assert(!validateDigiKeyJsonPayload(oversizedNumber, &error));
    assert(error.find("number") != string::npos);

    const string oversizedPayload(4U * 1024U * 1024U + 1U, ' ');
    assert(!validateDigiKeyJsonPayload(oversizedPayload, &error));
    assert(error.find("4 MiB") != string::npos);
  }

#ifdef _WIN32
  {
    string error;
    const string oversizedField = "\"" + string(1024U * 1024U + 1U, 'x') + "\"\n";
    assert(parseCsv(oversizedField, ',', error).empty());
    assert(error.find("1 MiB") != string::npos);

    string oversizedRow = "field";
    for (size_t index = 0; index < 512; ++index) oversizedRow += ",x";
    oversizedRow.push_back('\n');
    assert(parseCsv(oversizedRow, ',', error).empty());
    assert(error.find("too many fields") != string::npos);

    string oversizedRows = "field\n";
    oversizedRows.reserve(210000);
    for (size_t index = 0; index < 100000; ++index) oversizedRows += "x\n";
    assert(parseCsv(oversizedRows, ',', error).empty());
    assert(error.find("too many rows") != string::npos);

    const string oversizedText(25U * 1024U * 1024U + 1U, 'x');
    assert(parseCsv(oversizedText, ',', error).empty());
    assert(error.find("25 MiB") != string::npos);

    const string duplicateQuantityCsv =
        "Digi-Key Part Number,Manufacturer Part Number,Manufacturer,Description,Quantity\n"
        "123-ABC-ND,ABC-123,Acme,Overflow receipt,2147483647\n"
        "123-ABC-ND,ABC-123,Acme,Overflow receipt,1\n";
    const auto duplicateQuantity = parseDigiKeyCsvText(duplicateQuantityCsv, {});
    assert(duplicateQuantity.ok);
    assert(duplicateQuantity.candidates.size() == 1);
    assert(duplicateQuantity.candidates.front().item.quantity == numeric_limits<int>::max());
    assert(duplicateQuantity.candidates.front().warnings.size() == 1);

    const auto oversizedDigiKey = parseDigiKeyCsvText(oversizedText, {});
    assert(!oversizedDigiKey.ok);
    assert(oversizedDigiKey.error.find("25 MiB") != string::npos);

    const auto oversizedCsvPath = filesystem::temp_directory_path() / "inventatory-package-g-oversized.csv";
    error_code fileError;
    filesystem::remove(oversizedCsvPath, fileError);
    {
      ofstream output(oversizedCsvPath, ios::binary | ios::trunc);
      output << 'x';
    }
    filesystem::resize_file(oversizedCsvPath, 25U * 1024U * 1024U + 1U, fileError);
    assert(!fileError);
    const auto oversizedCsvFile = loadDigiKeyCsvFile(oversizedCsvPath, {});
    assert(!oversizedCsvFile.ok);
    assert(oversizedCsvFile.error.find("25 MiB") != string::npos);
    filesystem::remove(oversizedCsvPath, fileError);
    assert(!fileError);
  }

  {
    const string tooManyDesignators = [&] {
      string value = "Designator,Designation\n\"";
      for (size_t index = 0; index < 10001; ++index) {
        if (index != 0) value.push_back(',');
        value += "R" + to_string(index + 1);
      }
      value += "\",10k\n";
      return value;
    }();
    const auto designatorResult = parseKicadBomText(tooManyDesignators, "bounded");
    assert(!designatorResult.ok);
    assert(any_of(designatorResult.warnings.begin(), designatorResult.warnings.end(),
                  [](const string& warning) { return warning.find("too many designators") != string::npos; }));

    const auto oversizedQuantity = parseKicadBomText(
        "Designator,Designation,Quantity\nR1,10k,2147483648\n", "overflow");
    assert(!oversizedQuantity.ok);
    assert(!oversizedQuantity.warnings.empty());

    const string oversizedText(25U * 1024U * 1024U + 1U, 'x');
    const auto oversizedTextResult = parseKicadBomText(oversizedText, "oversized");
    assert(!oversizedTextResult.ok);
    assert(oversizedTextResult.error.find("25 MiB") != string::npos);

    const auto oversizedBomPath = filesystem::temp_directory_path() / "inventatory-package-g-oversized-bom.csv";
    error_code fileError;
    filesystem::remove(oversizedBomPath, fileError);
    {
      ofstream output(oversizedBomPath, ios::binary | ios::trunc);
      output << 'x';
    }
    filesystem::resize_file(oversizedBomPath, 25U * 1024U * 1024U + 1U, fileError);
    assert(!fileError);
    const auto oversizedBomFile = loadKicadBomFile(oversizedBomPath);
    assert(!oversizedBomFile.ok);
    assert(oversizedBomFile.error.find("25 MiB") != string::npos);
    filesystem::remove(oversizedBomPath, fileError);
    assert(!fileError);

    const auto oversizedNumber = packageFromFootprint("JST_PH_999999999999999999999x999999999999999999999");
    assert(oversizedNumber == "JST PH");
    assert(packageFromFootprint("JST_PH_B8B-PH-K_1x08_P2.00mm") == "JST PH 8");

    InventoryItem resistor;
    resistor.id = "bom-overflow-resistor";
    resistor.partName = "10k resistor 0603";
    resistor.category = "Resistors";
    resistor.quantity = numeric_limits<int>::max();
    resistor.parameters = {{"Resistance", "10k"}, {"Package", "0603"}};
    const vector<InventoryItem> items = {resistor};
    BomLine line;
    line.designators = {"R1"};
    line.footprint = "R_0603_1608Metric";
    line.designation = "10k";
    line.quantityPerBoard = numeric_limits<int>::max();
    KicadBomFile bom;
    bom.ok = true;
    bom.lines = {line, line};
    const auto analysis = analyzeBom(bom, items, 2, {});
    assert(analysis.matches.size() == 2);
    assert(analysis.matches[0].needed == numeric_limits<int>::max());
    assert(analysis.matches[1].needed == numeric_limits<int>::max());
    assert(analysis.totalPieces == numeric_limits<int>::max());
  }

  {
    assert(rackNumberFromCode("R") == 0);
    assert(rackNumberFromCode("X12") == 0);
    assert(rackNumberFromCode("R12x") == 0);
    assert(rackNumberFromCode("R2147483648") == 0);
    assert(rackNumberFromCode("R2147483647") == numeric_limits<int>::max());
    assert(rackNumberFromCode("r0012") == 12);

    InventoryStore normalized;
    InventatoryRack rack;
    rack.id = "rack-dimension-normalization";
    rack.code = "R9";
    rack.componentType = "Resistors";
    rack.rows = numeric_limits<int>::max();
    rack.columns = numeric_limits<int>::max();
    normalized.racks().push_back(rack);

    InventoryItem occupied;
    occupied.id = "normalized-occupied";
    occupied.rackId = rack.id;
    occupied.rackSlot = " a1 ";
    normalized.items().push_back(occupied);
    InventoryItem invalidSlot = occupied;
    invalidSlot.id = "normalized-invalid";
    invalidSlot.rackSlot = "F1";
    normalized.items().push_back(invalidSlot);
    InventoryItem invalidColumn = occupied;
    invalidColumn.id = "normalized-invalid-column";
    invalidColumn.rackSlot = "A6";
    normalized.items().push_back(invalidColumn);
    assert(rackOccupiedSlotCount(normalized, normalized.racks().front()) == 1);
    assert(itemAtRackSlot(normalized, rack.id, " A1 ") == &normalized.items().front());

    InventoryItem automatic;
    automatic.id = "normalized-automatic";
    automatic.partName = "10k resistor";
    automatic.category = "Resistors";
    automatic.parameters = {{"Package", "0603"}};
    automatic.rackId = rack.id;
    automatic.rackSlot = " b2 ";
    normalized.items().push_back(automatic);
    assert(reconcileRackAssignment(normalized, normalized.items().back()));
    assert(normalized.items().back().rackSlot == "B2");

    string error;
    assert(setManualRackLocation(normalized, normalized.items().back(), "r9-c3", error));
    assert(normalized.items().back().rackSlot == "C3");
    assert(!setManualRackLocation(normalized, normalized.items().back(), "r9-f1", error));
    assert(!moveItemToRackSlot(normalized, normalized.items().back(), normalized.racks().front(), "A6", error));

    InventoryStore noWrap;
    InventatoryRack maxRack;
    maxRack.id = "rack-max-code";
    maxRack.code = "R2147483647";
    maxRack.componentType = "Resistors";
    maxRack.rows = 0;
    maxRack.columns = 0;
    noWrap.racks().push_back(maxRack);
    InventoryItem pending;
    pending.id = "rack-no-wrap";
    pending.partName = "10k resistor";
    pending.category = "Resistors";
    pending.parameters = {{"Package", "0603"}};
    noWrap.items().push_back(pending);
    assert(!reconcileRackAssignment(noWrap, noWrap.items().back()));
    assert(noWrap.racks().size() == 1);
    assert(noWrap.items().back().rackAssignment == RackAssignmentMode::Automatic);
    assert(noWrap.items().back().rackId.empty());
  }

  {
    const auto path = filesystem::temp_directory_path() / "inventatory-package-g-quick-labels.conf";
    error_code cleanupError;
    filesystem::remove(path, cleanupError);
    assert(!saveQuickLabels(path, {"label"}, 0));
    assert(saveQuickLabels(path, {"label"}, numeric_limits<uint32_t>::max()));
    vector<string> loaded;
    uint32_t revision = 0;
    assert(loadQuickLabels(path, loaded, revision));
    assert(loaded == vector<string>({"label"}));
    assert(revision == numeric_limits<uint32_t>::max());

    const auto writeMalformed = [&](const string& contents) {
      ofstream output(path, ios::binary | ios::trunc);
      output << contents;
      output.close();
      loaded = {"preserved"};
      revision = 7;
      assert(!loadQuickLabels(path, loaded, revision));
      assert(loaded == vector<string>({"preserved"}));
      assert(revision == 7);
    };
    writeMalformed("quick_label_revision=0\n");
    writeMalformed("quick_label_revision=4294967296\n");
    writeMalformed("quick_label_revision=1 trailing-token\n");
    writeMalformed("quick_label_revision=1\nquick_label_revision=2\n");
    writeMalformed("quick_label_revision=1\nunknown=ignored\n");
    writeMalformed("quick_label_revision=1\nmalformed record\n");
    writeMalformed("quick_label_revision=1\nquick_label=\"unterminated\n");
    filesystem::remove(path, cleanupError);
  }
#endif
}

void testDeviceSyncRetryBackoff() {
  using app_actions::deviceSyncRetryDelay;
  assert(deviceSyncRetryDelay(1) == chrono::seconds(2));
  assert(deviceSyncRetryDelay(2) == chrono::seconds(4));
  assert(deviceSyncRetryDelay(3) == chrono::seconds(8));
  assert(deviceSyncRetryDelay(5) == chrono::seconds(32));
  assert(deviceSyncRetryDelay(6) == chrono::seconds(60));
  assert(deviceSyncRetryDelay(1000) == chrono::seconds(60));
  assert(deviceSyncRetryDelay(0) == chrono::seconds(2));
}

void testSettingsBridgeNotice() {
  using settings_page_detail::settingsBridgeNotice;
  const auto restarted = settingsBridgeNotice(DeviceServiceRestart::Restarted);
  assert(string(restarted.text) == "Settings saved; device bridge restarted");
  assert(restarted.severity == UiMessageSeverity::Success);
  // A workspace without a usable pairing is not a failed restart.
  const auto unpaired = settingsBridgeNotice(DeviceServiceRestart::DisabledUntilPaired);
  assert(string(unpaired.text).find("could not restart") == string::npos);
  assert(string(unpaired.text).find("until this workspace is paired") != string::npos);
  assert(unpaired.severity == UiMessageSeverity::Info);
  const auto failed = settingsBridgeNotice(DeviceServiceRestart::Failed);
  assert(string(failed.text).find("could not restart") != string::npos);
  assert(failed.severity == UiMessageSeverity::Warning);
}

// Negative Scan R1 transport cases: every rejected request must leave the sync callback untouched and
// must not consume the replay counter; a stale lower counter is rejected like an exact replay.
void testScannerHttpNegativeCases() {
  const auto stateDirectory = filesystem::temp_directory_path() / "inventatory-http-negative-test";
  error_code cleanupError;
  filesystem::remove_all(stateDirectory, cleanupError);
  const string token = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  const string deviceId = "r1-secure";
  const string body =
      R"({"protocolVersion":1,"requestId":"negative-sync","deviceId":"r1-secure","firmwareVersion":"0.1.0","mode":"ready","rssi":-40,"queueDepth":0,"events":[],"resultAcks":[]})";
  atomic<int> syncCalls{0};
  auto onSync = [&syncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response, string&) {
    ++syncCalls;
    response.requestId = request.requestId;
    return true;
  };
  LocalHttpServer server;
  server.setDeviceCredentials(deviceId, token, stateDirectory / "replay.state");
  assert(server.start(19510, onSync));
  const auto port = server.port();

  const auto expectRejected = [&](const string& request, const char* statusLine) {
    const auto response = sendLocalHttpRequest(port, request);
    assert(response.rfind(statusLine, 0) == 0);
    assert(syncCalls == 0);
  };
  // Replaces the value of one header line in a request built by signedSyncRequest.
  const auto withHeader = [](string request, const string& name, const string& value) {
    const auto begin = request.find(name + ": ");
    assert(begin != string::npos);
    const auto valueBegin = begin + name.size() + 2U;
    const auto end = request.find("\r\n", valueBegin);
    assert(end != string::npos);
    request.replace(valueBegin, end - valueBegin, value);
    return request;
  };
  const auto withoutHeader = [](string request, const string& name) {
    const auto begin = request.find(name + ": ");
    assert(begin != string::npos);
    const auto end = request.find("\r\n", begin);
    assert(end != string::npos);
    request.erase(begin, end + 2U - begin);
    return request;
  };
  const string syncPath = "/api/v1/device/sync";

  // Routing: only POST on the sync path is served; everything else is a 404.
  expectRejected("GET " + syncPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found");
  expectRejected("PUT " + syncPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found");
  expectRejected("POST /api/v1/device/unknown HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
                 "HTTP/1.1 404 Not Found");
  expectRejected("GET /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found");

  // Framing: chunked bodies and oversized declared bodies are refused before any authentication.
  expectRejected("POST " + syncPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n",
                 "HTTP/1.1 400 Bad Request");
  expectRejected("POST " + syncPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 1073741824\r\n\r\n",
                 "HTTP/1.1 413 Payload Too Large");

  // Authentication: each mutated request must be a 401 and must not reach the callback.
  const auto base = signedSyncRequest(token, deviceId, 70, body);
  const string protocolValue = to_string(kInventatoryScanTransportProtocolVersion);
  expectRejected(withHeader(base, "X-Inventatory-Protocol", protocolValue + "9"), "HTTP/1.1 401 Unauthorized");
  expectRejected(withoutHeader(base, "X-Inventatory-Protocol"), "HTTP/1.1 401 Unauthorized");
  expectRejected(withoutHeader(base, "X-Inventatory-Device"), "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Counter", "abc"), "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Counter", "99999999999999999999999"), "HTTP/1.1 401 Unauthorized");
  expectRejected(signedSyncRequest(token, deviceId, 0, body), "HTTP/1.1 401 Unauthorized");  // counter 0 is invalid

  const auto mac = deviceRequestMac(token, "POST", syncPath, deviceId, 70, body);
  assert(mac.size() == 64U);
  expectRejected(withHeader(base, "X-Inventatory-Mac", mac.substr(0, mac.size() - 2U)), "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Mac", mac + "00"), "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Mac", string(mac.size(), 'z')), "HTTP/1.1 401 Unauthorized");
  string upperMac = mac;
  for (auto& character : upperMac) {
    if (character >= 'a' && character <= 'f') character = static_cast<char>(character - 'a' + 'A');
  }
  if (upperMac != mac) expectRejected(withHeader(base, "X-Inventatory-Mac", upperMac), "HTTP/1.1 401 Unauthorized");

  // A MAC computed for a different path, method or body is not valid for this request.
  expectRejected(withHeader(base, "X-Inventatory-Mac",
                            deviceRequestMac(token, "POST", "/api/v1/device/other", deviceId, 70, body)),
                 "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Mac", deviceRequestMac(token, "GET", syncPath, deviceId, 70, body)),
                 "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Mac", deviceRequestMac(token, "POST", syncPath, deviceId, 70, body + " ")),
                 "HTTP/1.1 401 Unauthorized");
  expectRejected(withHeader(base, "X-Inventatory-Mac", deviceRequestMac(token, "POST", syncPath, deviceId, 71, body)),
                 "HTTP/1.1 401 Unauthorized");

  // Authenticated but wrong identity: the device header differs from the paired device, or the body
  // device id differs from the header. Both are rejected before the counter is reserved.
  expectRejected(signedSyncRequest(token, "r1-other", 72, body), "HTTP/1.1 400 Bad Request");
  string otherDeviceBody = body;
  const auto deviceInBody = otherDeviceBody.find("r1-secure");
  assert(deviceInBody != string::npos);
  otherDeviceBody.replace(deviceInBody, 9U, "r1-other");
  expectRejected(signedSyncRequest(token, deviceId, 73, otherDeviceBody), "HTTP/1.1 400 Bad Request");

  // An invalid or empty token never yields a MAC, so nothing can authenticate against it.
  assert(deviceRequestMac("", "POST", syncPath, deviceId, 1, body).empty());
  assert(deviceRequestMac("zz", "POST", syncPath, deviceId, 1, body).empty());

  // None of the rejections above consumed a counter: counter 100 is accepted, a stale lower counter and
  // an exact replay are 409 conflicts, and a higher counter is accepted again.
  const auto accepted = sendLocalHttpRequest(port, signedSyncRequest(token, deviceId, 100, body));
  assert(accepted.rfind("HTTP/1.1 200 OK", 0) == 0);
  assert(syncCalls == 1);
  const auto stale = sendLocalHttpRequest(port, signedSyncRequest(token, deviceId, 99, body));
  assert(stale.rfind("HTTP/1.1 409 Conflict", 0) == 0);
  const auto replay = sendLocalHttpRequest(port, signedSyncRequest(token, deviceId, 100, body));
  assert(replay.rfind("HTTP/1.1 409 Conflict", 0) == 0);
  assert(syncCalls == 1);
  const auto next = sendLocalHttpRequest(port, signedSyncRequest(token, deviceId, 101, body));
  assert(next.rfind("HTTP/1.1 200 OK", 0) == 0);
  assert(syncCalls == 2);

  server.stop();
  filesystem::remove_all(stateDirectory, cleanupError);
}

#ifndef _WIN32
void ignoreTestSignal(int) {}

vector<long> currentTaskIds() {
  vector<long> ids;
  error_code ec;
  for (const auto& entry : filesystem::directory_iterator("/proc/self/task", ec)) {
    ids.push_back(strtol(entry.path().filename().string().c_str(), nullptr, 10));
  }
  return ids;
}

// A signal delivered to the reader or a worker thread (terminal resize, a second launch asking this
// instance to come forward) interrupts select()/recv()/send() with EINTR. That must not drop the
// pending device request or lose the response.
void testScannerServerSurvivesSignals() {
  const auto stateDirectory = filesystem::temp_directory_path() / "inventatory-http-signal-test";
  error_code cleanupError;
  filesystem::remove_all(stateDirectory, cleanupError);
  const string token = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  const string deviceId = "r1-secure";
  const string body =
      R"({"protocolVersion":1,"requestId":"signal-sync","deviceId":"r1-secure","firmwareVersion":"0.1.0","mode":"ready","rssi":-40,"queueDepth":0,"events":[],"resultAcks":[]})";
  atomic<int> syncCalls{0};
  auto onSync = [&syncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response, string&) {
    ++syncCalls;
    response.requestId = request.requestId;
    return true;
  };

  struct sigaction handler{};
  handler.sa_handler = ignoreTestSignal;
  sigemptyset(&handler.sa_mask);
  handler.sa_flags = 0;  // deliberately no SA_RESTART
  struct sigaction previous{};
  assert(sigaction(SIGUSR1, &handler, &previous) == 0);

  const auto tasksBefore = currentTaskIds();
  LocalHttpServer server;
  server.setDeviceCredentials(deviceId, token, stateDirectory / "replay.state");
  assert(server.start(19483, onSync));
  vector<long> serverTasks;
  for (const long id : currentTaskIds()) {
    if (find(tasksBefore.begin(), tasksBefore.end(), id) == tasksBefore.end()) serverTasks.push_back(id);
  }
  assert(!serverTasks.empty());

  atomic<bool> signalling{true};
  thread signaller([&] {
    while (signalling.load()) {
      for (const long id : serverTasks) syscall(SYS_tgkill, getpid(), id, SIGUSR1);
      this_thread::sleep_for(chrono::milliseconds(2));
    }
  });

  const string request = signedSyncRequest(token, deviceId, 1, body);
  NativeSocket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(client != kInvalidSocket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  assert(setScannerTestAddress(address));
  address.sin_port = htons(server.port());
  assert(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
  const size_t half = request.size() / 2;
  assert(send(client, request.data(), half, 0) == static_cast<ssize_t>(half));
  this_thread::sleep_for(chrono::milliseconds(300));  // many signals hit the reader meanwhile
  assert(send(client, request.data() + half, request.size() - half, 0) ==
         static_cast<ssize_t>(request.size() - half));
  timeval timeout{};
  timeout.tv_sec = 3;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  string response;
  array<char, 1024> buffer{};
  ssize_t received = 0;
  while ((received = recv(client, buffer.data(), buffer.size(), 0)) > 0) {
    response.append(buffer.data(), static_cast<size_t>(received));
  }
  closeSocket(client);
  signalling.store(false);
  signaller.join();
  assert(response.rfind("HTTP/1.1 200 OK", 0) == 0);
  assert(syncCalls == 1);

  server.stop();
  assert(sigaction(SIGUSR1, &previous, nullptr) == 0);
  filesystem::remove_all(stateDirectory, cleanupError);
}
#endif

void testScannerCredentialResolution() {
  using namespace app_actions;

  const string validLower(64, 'a');
  const string validUpper(64, 'F');
  assert(validScannerToken(validLower));
  assert(validScannerToken(validUpper));
  assert(!validScannerToken(""));
  assert(!validScannerToken(string(63, '0')));
  assert(!validScannerToken(string(65, '0')));
  assert(!validScannerToken(string(63, '0') + 'g'));
  assert(!validScannerToken(string(63, '0') + ' '));

  // Every branch of the pure resolution, including a credential store that cannot answer.
  {
    struct Row {
      CredentialReadStatus lookup;
      optional<string> secret;
      bool setupComplete;
      string deviceId;
      WorkspaceScannerCredentialStatus expected;
      bool expectUnavailable;
    };
    const vector<Row> table = {
        {CredentialReadStatus::Found, validLower, false, "", WorkspaceScannerCredentialStatus::Loaded, false},
        {CredentialReadStatus::Found, validLower, true, "r1", WorkspaceScannerCredentialStatus::Loaded, false},
        {CredentialReadStatus::Found, "short", false, "", WorkspaceScannerCredentialStatus::RequiresPairing, false},
        {CredentialReadStatus::Found, nullopt, false, "", WorkspaceScannerCredentialStatus::RequiresPairing, false},
        {CredentialReadStatus::NotFound, nullopt, false, "", WorkspaceScannerCredentialStatus::FreshCredential, false},
        {CredentialReadStatus::NotFound, nullopt, false, "  ", WorkspaceScannerCredentialStatus::FreshCredential, false},
        {CredentialReadStatus::NotFound, nullopt, true, "", WorkspaceScannerCredentialStatus::RequiresPairing, false},
        {CredentialReadStatus::NotFound, nullopt, false, "r1", WorkspaceScannerCredentialStatus::RequiresPairing, false},
        // An unreadable store never yields a fresh credential (it would replace the real token) and is
        // reported as unavailable, whether or not a pairing is recorded.
        {CredentialReadStatus::Unavailable, nullopt, false, "", WorkspaceScannerCredentialStatus::RequiresPairing, true},
        {CredentialReadStatus::Unavailable, nullopt, true, "r1", WorkspaceScannerCredentialStatus::RequiresPairing, true},
        {CredentialReadStatus::Unavailable, validLower, false, "", WorkspaceScannerCredentialStatus::RequiresPairing, true},
    };
    for (const auto& row : table) {
      InventatoryScanConfig rowConfig;
      rowConfig.setupComplete = row.setupComplete;
      rowConfig.deviceId = row.deviceId;
      const auto outcome = resolveScannerCredential(CredentialLookup{row.lookup, row.secret}, rowConfig);
      assert(outcome.status == row.expected);
      assert(outcome.credentialStoreUnavailable == row.expectUnavailable);
      assert(outcome.token.has_value() == (row.expected == WorkspaceScannerCredentialStatus::Loaded));
    }
  }

  const auto testBase = filesystem::temp_directory_path() / "inventatory-credential-resolution-test";
  const auto workspace = testBase / ("workspace-" + to_string(static_cast<unsigned long long>(
                                                        chrono::steady_clock::now().time_since_epoch().count())));
  error_code cleanupError;
  filesystem::remove_all(testBase, cleanupError);
  assert(filesystem::create_directories(workspace));

  struct CleanupGuard {
    filesystem::path ws;
    filesystem::path base;
    ~CleanupGuard() {
      CredentialStore::eraseForWorkspace(ws, kInventatoryScanTokenCredential);
      error_code ec;
      filesystem::remove_all(base, ec);
    }
  } cleanupGuard{workspace, testBase};
  CredentialStore::eraseForWorkspace(workspace, kInventatoryScanTokenCredential);

  InventatoryScanConfig config;
  auto res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::FreshCredential && !res.token.has_value());

  config = {};
  config.setupComplete = true;
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::RequiresPairing && !res.token.has_value());

  config = {};
  config.deviceId = "r1-secure";
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::RequiresPairing && !res.token.has_value());

  config = {};
  config.deviceId = "   ";
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::FreshCredential && !res.token.has_value());

  assert(CredentialStore::writeForWorkspace(workspace, kInventatoryScanTokenCredential, validLower));
  config = {};
  config.setupComplete = false;
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::Loaded && res.token.has_value() && *res.token == validLower);

  config = {};
  config.setupComplete = true;
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::Loaded && res.token.has_value() && *res.token == validLower);
  assert(CredentialStore::eraseForWorkspace(workspace, kInventatoryScanTokenCredential));

  assert(CredentialStore::writeForWorkspace(workspace, kInventatoryScanTokenCredential, "not-a-valid-token"));
  config = {};
  res = resolveWorkspaceScannerCredential(workspace, config);
  assert(res.status == WorkspaceScannerCredentialStatus::RequiresPairing && !res.token.has_value());
  assert(CredentialStore::eraseForWorkspace(workspace, kInventatoryScanTokenCredential));

  filesystem::remove_all(testBase, cleanupError);
  assert(!cleanupError);
}

// Scanner events are committed against the database while the UI may hold unsaved edits, an open
// edit form or a staged import. These tests cover the core units those App paths rely on.
namespace {

InventoryItem mergeTestItem(const string& id, const string& name, int quantity) {
  InventoryItem item;
  item.id = id;
  item.partName = name;
  item.manufacturer = "Acme";
  item.category = "Resistors";
  item.quantity = quantity;
  item.lastUpdated = 1710000000;
  item.createdAt = 1700000000;
  return item;
}

// Gives every field of an item a value derived from `tag` so two fills never agree on any field.
// A field that mergeEditedItem forgets to carry over shows up as a serialized difference.
void fillMergeTestItem(InventoryItem& item, const string& tag, int number) {
  item.partName = tag + " name";
  item.manufacturer = tag + " maker";
  item.category = tag + " category";
  item.quantity = 100 + number;
  item.reorderThreshold = 200 + number;
  item.location = tag + " location";
  item.tags = {tag + "-tag-a", tag + "-tag-b"};
  item.parameters = {{tag + "-param", tag + "-value"}};
  item.notes = tag + " notes";
  item.digikeyPartNumber = tag + "-dk";
  item.datasheetUrl = "https://example.test/" + tag + "/datasheet";
  item.productUrl = "https://example.test/" + tag + "/product";
  item.syncStatus = tag + "-sync";
  item.sku = tag + "-sku";
  item.lastUpdated = 1710000000 + number;
  item.inventatoryId = "Inventatory:RES-0" + to_string(1000 + number);
  item.createdAt = 1700000000 + number;
  item.machineCode = to_string(7000 + number);
  item.rackId = tag + "-rack";
  item.rackSlot = number % 2 == 0 ? "A1" : "B2";
  item.rackAssignment = number % 2 == 0 ? RackAssignmentMode::Manual : RackAssignmentMode::Unassigned;
  item.labelOverride = tag + " label";
  item.vendorMetadata.provider = tag + "-provider";
  item.vendorMetadata.providerProductNumber = tag + "-product-number";
  item.vendorMetadata.manufacturerPartNumber = tag + "-mpn";
  item.vendorMetadata.categoryId = tag + "-category-id";
  item.vendorMetadata.categoryPath = {tag + "-path-a", tag + "-path-b"};
  item.vendorMetadata.title = tag + " title";
  item.vendorMetadata.detailedDescription = tag + " description";
  item.vendorMetadata.parameters = {{tag + "-vparam", tag + "-vvalue"}};
  item.vendorMetadata.productUrl = "https://example.test/" + tag + "/vendor";
  item.vendorMetadata.locale = tag + "-locale";
}

bool anyNoticeContains(const vector<string>& notices, const string& needle) {
  return any_of(notices.begin(), notices.end(),
                [&needle](const string& notice) { return notice.find(needle) != string::npos; });
}

}  // namespace

void testScannerCommitIgnoresUnsavedMemoryEdits() {
#ifdef INVENTATORY_SQLITE_STORAGE
  // A failed save keeps the edited store in memory for retry. A scanner event processed in that
  // window must not use the unsaved store as its diff baseline: the commit would count only the
  // scanner change while its snapshot also carried the edit, which fails history validation on
  // every later save and refuses to load on the next launch.
  const auto path = filesystem::temp_directory_path() / "inventatory-scanner-unsaved-edit-test.db";
  error_code cleanupError;
  filesystem::remove(path, cleanupError);

  InventoryStore persisted;
  persisted.items().push_back(mergeTestItem("unsaved-a", "Edited part", 5));
  persisted.items().push_back(mergeTestItem("unsaved-b", "Scanned part", 5));
  ensureInventoryIdentifiers(persisted.items());
  assert(persisted.save(path));
  assert(ensureInventoryCommitHistory(path, persisted));

  InventoryStore live = persisted;
  assert(!inventoryHasChanges(persisted, live));
  live.items()[0].partName = "Edited but not saved";
  live.items()[0].quantity = 9;
  assert(inventoryHasChanges(persisted, live));

  DeviceSyncRequest request;
  request.protocolVersion = 1;
  request.requestId = "unsaved-sync";
  request.deviceId = "device-a";
  request.events = {{"unsaved-event", "inventory.adjust", persisted.items()[1].machineCode, 1}};
  DeviceSyncResponse response;
  string error;
  assert(acceptDeviceSyncEvents(path, request, response, error));

  auto candidate = live;
  DeviceQuantityRequest quantityRequest{"device-a", "unsaved-event", persisted.items()[1].machineCode, 1};
  const auto applied = applyDeviceQuantity(candidate, quantityRequest);
  assert(applied.ok);
  DeviceSyncResult result;
  result.resultId = "unsaved-event-result";
  result.eventId = "unsaved-event";
  result.deviceId = "device-a";
  result.status = "completed";
  result.existing = true;
  result.itemName = applied.item;
  result.requestedDelta = 1;
  result.appliedDelta = applied.appliedDelta;
  result.quantity = applied.quantity;
  result.message = "Quantity updated";
  assert(completeDeviceSyncEvent(candidate, path, result, &live));

  vector<InventoryCommit> commits;
  assert(loadInventoryCommits(path, commits));  // validates every commit's counts against its snapshots
  assert(commits.size() == 2);
  InventoryCommitDetail detail;
  assert(loadInventoryCommit(path, commits.front().id, detail));
  assert(detail.hasParent);
  assert(inventoryCommitDiff(detail.snapshot, candidate).empty());
  // The stored count is the number of items that differ from the parent snapshot, here both the
  // scanned part and the part carrying the unsaved edit, whatever baseline the caller passed.
  unordered_set<string> changedItems;
  for (const auto& change : detail.changes) {
    if (change.entityType == "item") changedItems.insert(change.entityId);
  }
  assert(changedItems.size() == 2);
  assert(detail.commit.changedItemCount == changedItems.size());
  {
    SqliteConnection connection;
    assert(openDatabaseReadOnly(path, connection));
    string validationError;
    assert(validateInventoryDatabase(connection, &validationError));
  }

  // The inventory remains saveable afterwards and keeps validating.
  auto next = candidate;
  next.items()[1].quantity += 2;
  InventoryCommitDraft draft;
  draft.source = "manual";
  assert(next.saveWithCommit(path, candidate, draft));
  assert(loadInventoryCommits(path, commits));
  assert(commits.size() == 3);

  // A caller that believes something changed while the store already equals its parent snapshot
  // must not leave a change-less, non-checkpoint commit behind (history validation rejects it).
  InventoryCommit noOp;
  assert(next.saveWithCommit(path, persisted, draft, {}, nullptr, &noOp));
  assert(noOp.id.empty());
  assert(loadInventoryCommits(path, commits));
  assert(commits.size() == 3);

  filesystem::remove(path, cleanupError);
#endif
}

void testInventoryMerge() {
  // Edit form: the scanner changed quantity while the form was open.
  {
    const auto base = mergeTestItem("merge-a", "Resistor", 10);
    auto edited = base;
    edited.partName = "Resistor 10k";
    edited.notes = "checked";
    edited.lastUpdated = base.lastUpdated + 50;
    auto current = base;
    current.quantity = 14;
    current.lastUpdated = base.lastUpdated + 100;
    vector<string> notices;
    const auto merged = mergeEditedItem(base, edited, current, QuantityMerge::PreferEdited, &notices);
    assert(merged.id == "merge-a");
    assert(merged.partName == "Resistor 10k");
    assert(merged.notes == "checked");
    assert(merged.quantity == 14);  // the scanner's change survives a stale copy
    assert(merged.lastUpdated == base.lastUpdated + 100);
    assert(notices.empty());
  }
  // The user typed a quantity as well: the typed value wins and the replaced scanner value is reported.
  {
    const auto base = mergeTestItem("merge-b", "Capacitor", 10);
    auto edited = base;
    edited.quantity = 3;
    auto current = base;
    current.quantity = 14;
    vector<string> notices;
    const auto merged = mergeEditedItem(base, edited, current, QuantityMerge::PreferEdited, &notices);
    assert(merged.quantity == 3);
    assert(notices.size() == 1);
    assert(anyNoticeContains(notices, "Capacitor"));
    assert(anyNoticeContains(notices, "quantity"));
    assert(anyNoticeContains(notices, "14"));
  }
  // Both sides reached the same value: nothing to report.
  {
    const auto base = mergeTestItem("merge-c", "Inductor", 10);
    auto edited = base;
    edited.quantity = 12;
    auto current = base;
    current.quantity = 12;
    vector<string> notices;
    assert(mergeEditedItem(base, edited, current, QuantityMerge::PreferEdited, &notices).quantity == 12);
    assert(notices.empty());
  }
  // Every field is carried: an edit of all fields onto an untouched live item reproduces the edit,
  // and an untouched edit over a live item that changed every field reproduces the live item.
  {
    auto base = mergeTestItem("merge-d", "unused", 0);
    fillMergeTestItem(base, "base", 1);
    auto edited = base;
    fillMergeTestItem(edited, "user", 2);
    auto current = base;
    fillMergeTestItem(current, "scan", 3);

    vector<string> notices;
    auto onUntouched = mergeEditedItem(base, edited, base, QuantityMerge::PreferEdited, &notices);
    assert(notices.empty());
    assert(serializeItem(onUntouched) == serializeItem(edited));

    auto untouchedEdit = mergeEditedItem(base, base, current, QuantityMerge::PreferEdited, &notices);
    assert(notices.empty());
    assert(serializeItem(untouchedEdit) == serializeItem(current));

    auto conflicting = mergeEditedItem(base, edited, current, QuantityMerge::PreferEdited, &notices);
    assert(!notices.empty());
    // lastUpdated is the newer of the two and never a conflict.
    auto expected = edited;
    expected.lastUpdated = max(edited.lastUpdated, current.lastUpdated);
    assert(serializeItem(conflicting) == serializeItem(expected));
  }
  // Import: the quantity is stock added on top of the snapshot and composes with the scanner's.
  {
    const auto base = mergeTestItem("merge-e", "Diode", 5);
    auto staged = base;
    staged.quantity = 5 + 3;
    auto current = base;
    current.quantity = 5 + 4;
    vector<string> notices;
    assert(mergeEditedItem(base, staged, current, QuantityMerge::ApplyDelta, &notices).quantity == 12);
    assert(notices.empty());
    staged.quantity = numeric_limits<int>::max();
    current.quantity = numeric_limits<int>::max() - 1;
    assert(mergeEditedItem(base, staged, current, QuantityMerge::ApplyDelta, &notices).quantity ==
           numeric_limits<int>::max());
    staged.quantity = 0;
    current.quantity = 2;
    assert(mergeEditedItem(base, staged, current, QuantityMerge::ApplyDelta, &notices).quantity == 0);
  }

  // Import review: scanner quantity change and scanner-created item survive the staged commit.
  {
    InventoryStore base;
    base.items().push_back(mergeTestItem("store-x", "Stocked X", 5));
    base.items().push_back(mergeTestItem("store-z", "Stocked Z", 8));
    InventatoryRack rack;
    rack.id = "store-rack";
    rack.code = "R1";
    rack.componentType = "Resistors";
    base.racks().push_back(rack);

    auto staged = base;
    staged.items()[0].quantity += 3;  // import merged 3 into X
    staged.items()[1].notes = "enriched by import";
    staged.items().push_back(mergeTestItem("store-y", "Imported Y", 20));

    auto current = base;
    current.items()[0].quantity += 4;  // scanner received 4 of X
    current.items().push_back(mergeTestItem("store-s", "Scanner created S", 1));

    vector<string> notices;
    const auto merged = mergeInventoryChanges(base, staged, current, QuantityMerge::ApplyDelta, &notices);
    assert(notices.empty());
    assert(merged.items().size() == 4);
    assert(merged.findById("store-x") != nullptr && merged.findById("store-x")->quantity == 12);
    assert(merged.findById("store-z")->notes == "enriched by import");
    assert(merged.findById("store-y") != nullptr && merged.findById("store-y")->quantity == 20);
    assert(merged.findById("store-s") != nullptr && merged.findById("store-s")->quantity == 1);
    // Live items keep their order; staged additions are appended.
    assert(merged.items()[0].id == "store-x" && merged.items()[1].id == "store-z" &&
           merged.items()[2].id == "store-s" && merged.items()[3].id == "store-y");
    assert(merged.racks().size() == 1);

    // Nothing staged: the live store is returned untouched.
    vector<string> none;
    const auto unchanged = mergeInventoryChanges(base, base, current, QuantityMerge::ApplyDelta, &none);
    assert(none.empty());
    assert(inventoryCommitDiff(current, unchanged).empty());
    assert(unchanged.items().size() == current.items().size());
  }

  // Items that disappeared or collide are reported instead of being resurrected or duplicated.
  {
    InventoryStore base;
    base.items().push_back(mergeTestItem("gone", "Removed meanwhile", 5));
    base.items().push_back(mergeTestItem("kept", "Kept part", 5));
    base.items().push_back(mergeTestItem("drop", "Dropped by import", 5));
    auto staged = base;
    staged.items()[0].notes = "edited while it was removed";
    staged.items()[2].quantity = 99;
    staged.items().erase(staged.items().begin() + 2);  // staged removal of a part the scanner changed
    staged.items().push_back(mergeTestItem("twin", "Same id staged", 1));
    auto current = base;
    current.items().erase(current.items().begin());
    current.items()[1].quantity = 6;  // "drop" changed after the snapshot
    current.items().push_back(mergeTestItem("twin", "Same id live", 7));
    vector<string> notices;
    const auto merged = mergeInventoryChanges(base, staged, current, QuantityMerge::ApplyDelta, &notices);
    assert(merged.findById("gone") == nullptr);
    assert(anyNoticeContains(notices, "Removed meanwhile"));
    assert(merged.findById("drop") != nullptr && merged.findById("drop")->quantity == 6);
    assert(anyNoticeContains(notices, "Dropped by import"));
    size_t twins = 0;
    for (const auto& item : merged.items()) twins += item.id == "twin" ? 1 : 0;
    assert(twins == 1 && merged.findById("twin")->quantity == 7);
    assert(anyNoticeContains(notices, "twin") || anyNoticeContains(notices, "Same id"));
    // A staged removal of an unchanged live part applies.
    InventoryStore plainBase;
    plainBase.items().push_back(mergeTestItem("rm", "Plain removal", 5));
    InventoryStore plainStaged;
    vector<string> plainNotices;
    assert(mergeInventoryChanges(plainBase, plainStaged, plainBase, QuantityMerge::ApplyDelta, &plainNotices)
               .items()
               .empty());
    assert(plainNotices.empty());
  }

  // Racks created on both sides: a code collision keeps the live rack and re-places staged parts.
  {
    InventoryStore base;
    base.items().push_back(mergeTestItem("rack-item", "Placed by import", 1));
    InventatoryRack existing;
    existing.id = "rack-1";
    existing.code = "R1";
    existing.componentType = "Resistors";
    base.racks().push_back(existing);

    auto staged = base;
    InventatoryRack stagedRack;
    stagedRack.id = "rack-staged";
    stagedRack.code = "R2";
    stagedRack.componentType = "Resistors";
    staged.racks().push_back(stagedRack);
    InventatoryRack stagedOther;
    stagedOther.id = "rack-staged-other";
    stagedOther.code = "R3";
    stagedOther.componentType = "Capacitors";
    staged.racks().push_back(stagedOther);
    InventoryItem placed = mergeTestItem("import-new", "Imported on R2", 4);
    placed.rackId = "rack-staged";
    placed.rackSlot = "A1";
    placed.rackAssignment = RackAssignmentMode::Manual;
    staged.items().push_back(placed);
    InventoryItem placedOther = mergeTestItem("import-other", "Imported on R3", 4);
    placedOther.rackId = "rack-staged-other";
    placedOther.rackSlot = "A1";
    placedOther.rackAssignment = RackAssignmentMode::Manual;
    staged.items().push_back(placedOther);

    auto current = base;
    InventatoryRack scannerRack;
    scannerRack.id = "rack-scanner";
    scannerRack.code = "r2";  // same code as the staged rack, different id
    scannerRack.componentType = "Resistors";
    current.racks().push_back(scannerRack);

    vector<string> notices;
    const auto merged = mergeInventoryChanges(base, staged, current, QuantityMerge::ApplyDelta, &notices);
    size_t rackCount = 0;
    for (const auto& rack : merged.racks()) rackCount += toLower(rack.code) == "r2" ? 1 : 0;
    assert(rackCount == 1);
    assert(merged.racks().size() == 3);  // R1, scanner R2, staged R3
    assert(anyNoticeContains(notices, "R2"));
    assert(merged.findById("import-new")->rackId.empty());
    assert(merged.findById("import-new")->rackAssignment == RackAssignmentMode::Automatic);
    assert(merged.findById("import-other")->rackId == "rack-staged-other");
    assert(merged.findById("import-other")->rackSlot == "A1");
  }

  // A staged part placed in a slot the scanner filled meanwhile is placed again, not stacked on it.
  {
    InventoryStore base;
    InventatoryRack rack;
    rack.id = "slot-rack";
    rack.code = "R1";
    rack.componentType = "Resistors";
    base.racks().push_back(rack);
    auto staged = base;
    InventoryItem imported = mergeTestItem("slot-import", "Imported in A1", 2);
    imported.rackId = "slot-rack";
    imported.rackSlot = "A1";
    imported.rackAssignment = RackAssignmentMode::Manual;
    staged.items().push_back(imported);
    auto current = base;
    InventoryItem scanned = mergeTestItem("slot-scanned", "Scanned into A1", 1);
    scanned.rackId = "slot-rack";
    scanned.rackSlot = "a1";
    scanned.rackAssignment = RackAssignmentMode::Manual;
    current.items().push_back(scanned);
    const auto merged = mergeInventoryChanges(base, staged, current, QuantityMerge::ApplyDelta);
    assert(merged.findById("slot-scanned")->rackSlot == "a1");
    assert(merged.findById("slot-import")->rackId.empty());
    assert(merged.findById("slot-import")->rackAssignment == RackAssignmentMode::Automatic);
    // Without a collision the staged placement is kept.
    const auto uncontested = mergeInventoryChanges(base, staged, base, QuantityMerge::ApplyDelta);
    assert(uncontested.findById("slot-import")->rackId == "slot-rack" &&
           uncontested.findById("slot-import")->rackSlot == "A1");
  }

  // End to end: the scanner commits during the staged work, then the merged store is saved the
  // way the import commit does it. History stays valid and keeps both changes.
  {
#ifdef INVENTATORY_SQLITE_STORAGE
    const auto path = filesystem::temp_directory_path() / "inventatory-merge-scanner-import-test.db";
    error_code cleanupError;
    filesystem::remove(path, cleanupError);

    InventoryStore persisted;
    persisted.items().push_back(mergeTestItem("e2e-x", "Scanned X", 5));
    persisted.items().push_back(mergeTestItem("e2e-z", "Plain Z", 8));
    ensureInventoryIdentifiers(persisted.items());
    assert(persisted.save(path));
    assert(ensureInventoryCommitHistory(path, persisted));

    const auto base = persisted;  // snapshot taken when the import review starts
    auto staged = base;
    staged.items()[0].quantity += 3;
    staged.items().push_back(mergeTestItem("e2e-y", "Imported Y", 20));

    // The scanner receives 4 of X and creates a new part while the review is open.
    DeviceSyncRequest request;
    request.protocolVersion = 1;
    request.requestId = "e2e-sync";
    request.deviceId = "device-a";
    request.events = {{"e2e-event-1", "inventory.receive", persisted.items()[0].machineCode, 4},
                      {"e2e-event-2", "inventory.receive", "NEW-SCANNED-CODE", 2}};
    DeviceSyncResponse response;
    string error;
    assert(acceptDeviceSyncEvents(path, request, response, error));
    auto live = persisted;
    for (const auto& event : loadPendingDeviceSyncEvents(path, 8)) {
      auto candidate = live;
      const auto resolution = resolveScanCode(candidate, event.code);
      assert(resolution.matched);
      auto* item = candidate.findById(resolution.itemId);
      assert(item != nullptr);
      item->quantity += event.value;
      DeviceSyncResult result;
      result.resultId = event.eventId + "-result";
      result.eventId = event.eventId;
      result.deviceId = event.deviceId;
      result.status = "completed";
      result.itemName = item->partName;
      result.requestedDelta = event.value;
      result.appliedDelta = event.value;
      result.quantity = item->quantity;
      assert(completeDeviceSyncEvent(candidate, path, result, &live));
      live = move(candidate);
    }
    assert(live.items().size() == 3);

    // Import commit: replay the staged delta over the live store and save against the persisted store.
    vector<string> notices;
    auto merged = mergeInventoryChanges(base, staged, live, QuantityMerge::ApplyDelta, &notices);
    assert(notices.empty());
    assert(merged.findById("e2e-x")->quantity == 5 + 4 + 3);
    assert(merged.findById("e2e-y") != nullptr);
    assert(merged.items().size() == 4);
    ensureInventoryIdentifiers(merged.items());
    InventoryCommitDraft draft;
    draft.source = "import";
    draft.reference = "e2e.csv";
    assert(merged.saveWithCommit(path, live, draft));

    vector<InventoryCommit> commits;
    assert(loadInventoryCommits(path, commits));  // validates the whole history
    assert(commits.size() == 4);                  // initial, two scanner events, import
    assert(commits.front().source == "import" && commits.front().changedItemCount == 2);
    InventoryStore reloaded;
    assert(reloaded.load(path));
    assert(reloaded.findById("e2e-x")->quantity == 12);
    assert(reloaded.findById("e2e-y") != nullptr);
    assert(reloaded.items().size() == 4);
    filesystem::remove(path, cleanupError);
#endif
  }
}

int main() {
  testHistoryPagePresentationData();
#ifndef _WIN32
  testAtomicFileLinks();
#endif
  testHistoryPageLayoutData();
  testPrimaryNavigationContract();
#ifndef _WIN32
  testLinuxMdnsRefusesNonPrivateInterfaces();
#endif

  {
    const auto info = uiMessagePresentation(UiMessageSeverity::Info);
    const auto success = uiMessagePresentation(UiMessageSeverity::Success);
    const auto warning = uiMessagePresentation(UiMessageSeverity::Warning);
    const auto error = uiMessagePresentation(UiMessageSeverity::Error);
    const auto legacy = uiMessagePresentation(kLegacyMessageSeverity);
    assert(string(info.prefix) == "[i] ");
    assert(info.color == UiMessageColorRole::Info);
    assert(string(success.prefix) == "[ok] ");
    assert(success.color == UiMessageColorRole::Success);
    assert(string(warning.prefix) == "[!] ");
    assert(warning.color == UiMessageColorRole::Warning);
    assert(string(error.prefix) == "[x] ");
    assert(error.color == UiMessageColorRole::Error);
    assert(string(legacy.prefix) == "[i] ");
    assert(legacy.color == UiMessageColorRole::Info);
    assert(uiMessageAcknowledgementPulseActive(100, 100));
    assert(uiMessageAcknowledgementPulseActive(100, 299));
    assert(!uiMessageAcknowledgementPulseActive(100, 300));
    assert(!uiMessageAcknowledgementPulseActive(-1, 100));
    assert(uiMessageRowHeight() == 1);
  }

  {
    assert(rack_page_detail::equalRackSlotHeight(39, 5) == 7);
    assert(rack_page_detail::equalRackSlotHeight(40, 5) == 8);
    assert(rack_page_detail::equalRackSlotHeight(15, 5) == 3);
    assert(rack_page_detail::equalRackSlotHeight(16, 5) == 3);
    assert(rack_page_detail::equalRackSlotWidth(35, 5) == 7);
    assert(rack_page_detail::equalRackSlotWidth(39, 5) == 7);
    assert(rack_page_detail::equalRackSlotWidth(40, 5) == 8);
    assert(rack_page_detail::equalRackSlotWidth(100, 5) == 20);

    // Floor division ensures rack slots never exceed available space across terminal heights
    for (int availableRows = 15; availableRows <= 100; ++availableRows) {
      const int height = rack_page_detail::equalRackSlotHeight(availableRows, 5);
      assert(height >= 3);
      assert(height * 5 <= availableRows);
    }
    // Floor division ensures rack slot widths never exceed available space across terminal widths
    for (int availableCols = 35; availableCols <= 250; ++availableCols) {
      const int width = rack_page_detail::equalRackSlotWidth(availableCols, 5);
      assert(width >= 7);
      assert(width * 5 <= availableCols);
    }

    assert(rack_page_detail::shortComponentType("Integrated Circuits") == "ICs");
    assert(rack_page_detail::shortComponentType("integrated circuits") == "ICs");
    assert(rack_page_detail::shortComponentType("Integrated circuits") == "ICs");
    assert(rack_page_detail::shortComponentType("Integrated Circuits (ICs)") == "ICs");
    assert(rack_page_detail::shortComponentType("Resistors") == "Resistors");
    assert(rack_page_detail::shortComponentType("Transistors") == "Transistors");
  }

  {
    const auto categories = settings_page_detail::settingsCategoryEntries();
    assert(categories.size() == 7);
    assert(categories[0].categoryIndex == 0);
    assert(categories[1].categoryIndex == 1);
    assert(categories[2].categoryIndex == 2);
    assert(categories[3].categoryIndex == 3);
    assert(categories[4].categoryIndex == 4);
    assert(categories[5].categoryIndex == 5);
    assert(categories[6].categoryIndex == 6);
    assert(categories[3].indent == 0);
    assert(categories[4].indent == 1);
    assert(categories[5].indent == 0);
  }

  {
    const auto first = advanceWorkspaceGeneration(0);
    const auto second = advanceWorkspaceGeneration(first);
    assert(first != 0);
    assert(second != first);
    assert(workspaceGenerationMatches(first, first));
    assert(!workspaceGenerationMatches(second, first));
    assert(!workspaceGenerationMatches(0, first));
    assert(advanceWorkspaceGeneration(numeric_limits<WorkspaceGeneration>::max()) == 1);
    assert(bomEnrichmentScopeMatches("project-a", "project-a", first, first, 7, 7));
    assert(!bomEnrichmentScopeMatches("project-b", "project-a", first, first, 7, 7));
    assert(!bomEnrichmentScopeMatches("project-a", "project-a", second, first, 7, 7));
    assert(!bomEnrichmentScopeMatches("project-a", "project-a", first, first, 8, 7));
    assert(!bomEnrichmentScopeMatches("", "project-a", first, first, 7, 7));
    const DeviceQuickLabelPrintResult pending{"request-1", "pending", "queued", "poll again"};
    const DeviceQuickLabelPrintResult completed{"request-1", "completed", "", "Label sent"};
    const DeviceQuickLabelPrintResult failed{"request-1", "failed", "printer_failed", "Printer failed"};
    assert(!quickLabelResultIsTerminal(pending));
    assert(quickLabelResultIsTerminal(completed));
    assert(quickLabelResultIsTerminal(failed));
    assert(quickLabelResultMatchesRequest(pending, "request-1"));
    assert(!quickLabelResultMatchesRequest(pending, "request-2"));
    QuickLabelPrintCacheIdentity labelIdentity;
    labelIdentity.request = {"request-1", 2, 3};
    labelIdentity.deviceId = "r1-a";
    labelIdentity.labelText = "GND";
    labelIdentity.workspaceGeneration = first;
    assert(quickLabelPrintCacheIdentityMatches(labelIdentity, labelIdentity));
    auto changedLabelDevice = labelIdentity;
    changedLabelDevice.deviceId = "r1-b";
    assert(!quickLabelPrintCacheIdentityMatches(labelIdentity, changedLabelDevice));
    auto changedLabelRevision = labelIdentity;
    changedLabelRevision.request.revision = 4;
    assert(!quickLabelPrintCacheIdentityMatches(labelIdentity, changedLabelRevision));
    auto changedLabelPayload = labelIdentity;
    changedLabelPayload.request.presetIndex = 3;
    changedLabelPayload.labelText = "VCC";
    assert(!quickLabelPrintCacheIdentityMatches(labelIdentity, changedLabelPayload));
    auto changedLabelWorkspace = labelIdentity;
    changedLabelWorkspace.workspaceGeneration = second;
    assert(!quickLabelPrintCacheIdentityMatches(labelIdentity, changedLabelWorkspace));
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

  {
    const string key = "release-readiness-scanner-scope-test-" +
                       to_string(static_cast<unsigned long long>(chrono::steady_clock::now().time_since_epoch().count()));
    const auto scopeRoot = filesystem::temp_directory_path() / ("inventatory-scanner-scope-" + key);
    const auto workspaceA = scopeRoot / "workspace-a";
    const auto workspaceB = scopeRoot / "workspace-b";
    error_code cleanupError;
    filesystem::remove_all(scopeRoot, cleanupError);
    assert(filesystem::create_directories(workspaceA));
    assert(filesystem::create_directories(workspaceB));

    // A global target must never be implicitly visible through a workspace
    // scope. Scanner pairing is deliberately workspace-local.
    const auto legacyKey = key + "-legacy";
    CredentialStore::erase(legacyKey);
    CredentialStore::eraseForWorkspace(workspaceA, key);
    CredentialStore::eraseForWorkspace(workspaceB, key);
    assert(CredentialStore::write(legacyKey, "legacy-token"));
    assert(!CredentialStore::readForWorkspace(workspaceA, key).has_value());
    assert(!CredentialStore::readForWorkspace(workspaceB, key).has_value());

    assert(CredentialStore::workspaceScopedKey(workspaceA, key) !=
           CredentialStore::workspaceScopedKey(workspaceB, key));
    assert(CredentialStore::writeForWorkspace(workspaceA, key, "workspace-a-token"));
    assert(CredentialStore::writeForWorkspace(workspaceB, key, "workspace-b-token"));
    const auto scopedA = CredentialStore::readForWorkspace(workspaceA, key);
    const auto scopedB = CredentialStore::readForWorkspace(workspaceB, key);
    assert(scopedA.has_value() && *scopedA == "workspace-a-token");
    assert(scopedB.has_value() && *scopedB == "workspace-b-token");
    assert(CredentialStore::readForWorkspace(workspaceA / "child" / "..", key).has_value());

    const auto replayA = inventatoryScanReplayStatePath(workspaceA);
    const auto replayB = inventatoryScanReplayStatePath(workspaceB);
    assert(replayA != replayB);
    assert(replayA.parent_path() == workspaceA);
    assert(replayB.parent_path() == workspaceB);
    assert(inventatoryScanReplayStatePath(workspaceA / "." / "nested" / "..") == replayA);
    assert(inventatoryScanReplayStatePath({}).empty());

    assert(CredentialStore::eraseForWorkspace(workspaceA, key));
    assert(CredentialStore::eraseForWorkspace(workspaceB, key));
    assert(CredentialStore::erase(legacyKey));
    filesystem::remove_all(scopeRoot, cleanupError);
    assert(!cleanupError);
  }

#ifdef INVENTATORY_SQLITE_STORAGE
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
  testPhysicalValueCommaDecimalLocale();
  testDecimalParsingCommaLocale();
  testStockFilterState();
  testInventoryCommitHistory();
  testSqliteSchemaValidation();
  testPackageGHardening();
  testScannerCommitIgnoresUnsavedMemoryEdits();
  testInventoryMerge();

  {
#ifdef _WIN32
    assert(_putenv_s("INVENTATORY_TEST_ENVIRONMENT", "test-value") == 0);
#else
    assert(setenv("INVENTATORY_TEST_ENVIRONMENT", "test-value", 1) == 0);
#endif
    const auto value = environmentValue("INVENTATORY_TEST_ENVIRONMENT");
    assert(value.has_value());
    assert(*value == "test-value");
#ifdef _WIN32
    assert(_putenv_s("INVENTATORY_TEST_ENVIRONMENT", "") == 0);
#else
    assert(unsetenv("INVENTATORY_TEST_ENVIRONMENT") == 0);
#endif
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
    assert(stockFields.front().label == "Inventatory rack: ");
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

#ifdef INVENTATORY_SQLITE_STORAGE
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

#ifdef INVENTATORY_SQLITE_STORAGE
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
      string printedHeader = expected;
      transform(printedHeader.begin(), printedHeader.end(), printedHeader.begin(),
                [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
      assert(service.buildZpl(candidate).find("^FR^FD" + printedHeader + "^FS") != string::npos);
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
    assert(plan.mainIsMeasuredValue);
    assert(plan.mainValue == u8"10k\u03A9");
    assert(plan.mainTolerance == "1%");
    assert(plan.packageLine.empty());
    assert(plan.manufacturerLine == "Yageo");
    assert(plan.parameters.size() == 1);
    assert(labelTile(plan, "POWER") == "0.125W");
    assert(labelTile(plan, "RESISTANCE").empty());
    assert(plan.inventatoryId == "Inventatory:R-00123");
    assert(plan.scannerHint == "R-0002");
    assert(plan.barcodeHint == "0002");
    const auto zpl = service.buildZpl(item);
    assert(!zpl.empty());
    assert(zpl.find("10kOhms") == string::npos);
    assert(zpl.find("Tol ") == string::npos);
    assert(zpl.find("Tempco") == string::npos);
    assert(zpl.find("^FO13,5^A0N,16,16^FR^FDRESISTOR^FS") != string::npos);
    // Measured value: number, smaller unit and tolerance on one baseline, the
    // group centred between the header bar and the package row.
    assert(zpl.find("^FO30,34^A0N,46,46^FD10k^FS") != string::npos);
    assert(zpl.find(u8"^FD\u03A9^FS") != string::npos);
    assert(zpl.find("^A0N,18,18^FD1%^FS") != string::npos);
    assert(zpl.find("^FDPOWER^FS") != string::npos);
    assert(zpl.find("^FD0.125W^FS") != string::npos);
    assert(zpl.find("^FDYageo^FS") != string::npos);
    assert(zpl.find("^FO180,18^BQN,2,3^FDLA,0002^FS") != string::npos);
    // The short ID is printed under the QR code for manual lookup.
    // It is centred on the 63 dot QR symbol without printer-side centring.
    assert(zpl.find("^FO188,94^A0N,14,14^FDR-0002^FS") != string::npos);
    // Header bar starts at y 0 (prints land ~10 dots low) and carries the 5 x 5 logo.
    assert(zpl.find("^FO7,0^GB236,22,22,B,4^FS") != string::npos);
    assert(zplOccurrences(zpl, "^GB3,3,3^FS") == 10);
    assert(zpl.find("^FO224,4^FR^GB3,3,3^FS") != string::npos);
    assert(zpl.find("^FO236,10^FR^GB3,3,3^FS") != string::npos);
    // Without a rack slot the pen field is the only slot area.
    assert(zpl.find("^FDSLOT^FS") != string::npos);
    assert(zpl.find("^FDNEW SLOT^FS") == string::npos);
    assert(zpl.find("^FO180,111^GB63,23,2,B,3^FS") == string::npos);
    // The caption moves up into the chip's place and the pen field grows to fill it.
    assert(zpl.find("^FO180,111^A0N,11,11^FDSLOT^FS") != string::npos);
    assert(zpl.find("^FO180,123^GB7,2,2^FS") != string::npos);
    const auto rackPlan = service.buildLabelPlan(item, "R3-E3");
    assert(rackPlan.rackLocation == "R3-E3");
    assert(rackPlan.rackCode == "R3");
    assert(rackPlan.rackCell == "E3");
    const auto rackZpl = service.buildZpl(item, "R3-E3");
    assert(rackZpl.find("^FO180,111^GB63,23,2,B,3^FS") != string::npos);
    assert(rackZpl.find("^FO180,111^GB40,23,23,B,3^FS") != string::npos);
    assert(rackZpl.find("^FO190,116^A0N,17,17^FR^FDR3^FS") != string::npos);
    assert(rackZpl.find("^FO221,115^A0N,19,19^FDE3^FS") != string::npos);
    assert(rackZpl.find("^FDNEW SLOT^FS") != string::npos);
    // The pen field is plain corner brackets: no tick marks at the rack/cell split.
    assert(rackZpl.find("^GB1,5,1^FS") == string::npos);
    assert(zplOccurrences(rackZpl, "^GB7,2,2^FS") == 4);
    assert(zplOccurrences(rackZpl, "^GB2,7,2^FS") == 4);
    // Three-digit racks stay inside the black half of the slot chip.
    const auto wideRackPlan = service.buildLabelPlan(item, "R123-b2");
    assert(wideRackPlan.rackCode == "R123");
    assert(wideRackPlan.rackCell == "B2");
    const auto wideRackZpl = service.buildZpl(item, "R123-b2");
    const auto wideRackField = wideRackZpl.find("^FR^FDR123^FS");
    assert(wideRackField != string::npos);
    const auto wideRackFont = wideRackZpl.rfind("^A0N,", wideRackField);
    const auto wideRackSize = stoi(wideRackZpl.substr(wideRackFont + 5));
    assert(wideRackSize >= 13 && wideRackSize <= 17);
    assert(estimateFont0Width("R123", wideRackSize, wideRackSize) <= 35);
    assert(wideRackZpl.find("^FDB2^FS") != string::npos);
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
    assert(rackLabelPlan.shortCategoryText.empty());
    assert(rackLabelPlan.rackNumber == "01");
    assert(rackLabelPlan.symbolKey == "resistor");
    const auto rackLabelZpl = service.buildRackLabelZpl(resistorRack);
    assert(rackLabelZpl.find("^FDINVENTATORY RACK^FS") != string::npos);
    assert(rackLabelZpl.find("^FDRESISTORS^FS") != string::npos);
    assert(rackLabelZpl.find("^FD01^FS") != string::npos);
    assert(rackLabelZpl.find("^FDRACK^FS") != string::npos);
    // The title starts at the left of the header bar and the brand mark sits at its right end.
    assert(rackLabelZpl.find("^FO7,0^GB236,22,22,B,4^FS") != string::npos);
    assert(rackLabelZpl.find("^FO224,4^FR^GB3,3,3^FS") != string::npos);
    assert(rackLabelZpl.find("^FO13,5^A0N,16,16^FR^FDINVENTATORY RACK^FS") != string::npos);
    // No horizontal division line between the symbol and the type name.
    assert(rackLabelZpl.find("^GB120,1,1^FS") == string::npos);
    // The IEC resistor is a 120 x 34 dot graphic, the ANSI zigzag 120 x 42.
    assert(rackLabelZpl.find("^GFA,510,510,15,") != string::npos);
    const auto usResistorZpl = service.buildRackLabelZpl(resistorRack, SymbolStandard::Us);
    assert(usResistorZpl.find("^GFA,630,630,15,") != string::npos);
    assert(usResistorZpl != rackLabelZpl);

    // Only the resistor and the fuse differ between the standards.
    const auto sameInBothStandards = [&](const string& type) {
      InventatoryRack rack;
      rack.code = "R5";
      rack.componentType = type;
      return service.buildRackLabelZpl(rack, SymbolStandard::Eu) == service.buildRackLabelZpl(rack, SymbolStandard::Us);
    };
    for (const auto* type : {"Capacitors", "Inductors", "Diodes", "Indicators", "Transistors", "Integrated Circuits",
                             "Timing", "Connectors", "smd widgets"}) {
      assert(sameInBothStandards(type));
    }
    assert(!sameInBothStandards("Fuses"));
    assert(!sameInBothStandards("resistor"));

    InventatoryRack customRack;
    customRack.id = "rack-custom-1";
    customRack.code = "R12";
    customRack.componentType = "smd widgets";
    const auto customRackPlan = service.buildRackLabelPlan(customRack);
    assert(customRackPlan.categoryText == "SMD WIDGETS");
    assert(customRackPlan.rackNumber == "12");
    assert(customRackPlan.symbolKey == "grid");
    const auto customRackZpl = service.buildRackLabelZpl(customRack);
    // A custom type has no electrical symbol: it gets the 5 x 5 grid, and its name splits rather than shrinks.
    assert(customRackZpl.find("^GFA,594,594,9,") != string::npos);
    assert(customRackZpl.find("^FDSMD^FS") != string::npos);
    assert(customRackZpl.find("^FDWIDGETS^FS") != string::npos);

    // The full name is used while it fits nicely; the short form takes over when it does not.
    InventatoryRack longRack;
    longRack.id = "rack-long-1";
    longRack.code = "R13";
    longRack.componentType = "integrated circuits";
    const auto longRackPlan = service.buildRackLabelPlan(longRack);
    assert(longRackPlan.categoryText == "INTEGRATED CIRCUITS");
    assert(longRackPlan.shortCategoryText == "ICs");
    const auto longRackZpl = service.buildRackLabelZpl(longRack);
    assert(longRackZpl.find("^FDICs^FS") != string::npos);
    assert(longRackZpl.find("INTEGRATED") == string::npos);
    const auto rackNameSize = [&](const string& type, const string& text) {
      InventatoryRack rack;
      rack.code = "R2";
      rack.componentType = type;
      const auto zpl = service.buildRackLabelZpl(rack);
      const auto field = zpl.find("^FD" + text + "^FS");
      assert(field != string::npos);
      return stoi(zpl.substr(zpl.rfind("^A0N,", field) + 5));
    };
    assert(rackNameSize("Indicators", "INDICATORS") >= 22);
    assert(rackNameSize("Capacitors", "CAPACITORS") >= 22);
    assert(rackNameSize("Timing", "TIMING") >= 22);
    InventatoryRack ledRack;
    ledRack.code = "R5";
    ledRack.componentType = "LEDs";
    assert(service.buildRackLabelPlan(ledRack).symbolKey == "led");

    // Every symbol is centred in the space between the header bar and the type name.
    for (const auto& [type, nameText] : vector<pair<string, string>>{{"Transistors", "TRANSISTORS"},
                                                                      {"Resistors", "RESISTORS"},
                                                                      {"Integrated Circuits", "ICs"},
                                                                      {"Fuses", "FUSES"}}) {
      InventatoryRack rack;
      rack.code = "R4";
      rack.componentType = type;
      for (const auto standard : {SymbolStandard::Eu, SymbolStandard::Us}) {
        const auto zpl = service.buildRackLabelZpl(rack, standard);
        const auto graphic = zpl.find("^GFA,");
        const auto origin = zpl.rfind("^FO", graphic);
        const auto symbolY = stoi(zpl.substr(zpl.find(',', origin) + 1));
        const auto bytes = stoi(zpl.substr(graphic + 5));
        const auto bytesPerRow = stoi(zpl.substr(zpl.find(',', zpl.find(',', graphic + 5) + 1) + 1));
        const auto nameField = zpl.find("^FD" + nameText + "^FS");
        assert(nameField != string::npos);
        const auto nameOrigin = zpl.rfind("^FO", nameField);
        const auto nameY = stoi(zpl.substr(zpl.find(',', nameOrigin) + 1));
        const auto symbolCenter = symbolY + bytes / bytesPerRow / 2.0;
        assert(abs(symbolCenter - (22 + nameY) / 2.0) <= 1.0);
      }
    }

    // Three-digit racks step down but never below 48 dots, and nothing leaves the safe area.
    InventatoryRack wideRack;
    wideRack.code = "R128";
    wideRack.componentType = "Transistors";
    const auto wideRackLabelZpl = service.buildRackLabelZpl(wideRack);
    const auto wideRackNumberField = wideRackLabelZpl.find("^FD128^FS");
    assert(wideRackNumberField != string::npos);
    const auto wideRackNumberSize = stoi(wideRackLabelZpl.substr(wideRackLabelZpl.rfind("^A0N,", wideRackNumberField) + 5));
    assert(wideRackNumberSize >= 48 && wideRackNumberSize < 88);
    assert(estimateFont0Width("128", wideRackNumberSize, wideRackNumberSize) <= 90);
    for (const auto* type : {"Resistors", "Capacitors", "Inductors", "Diodes", "Indicators", "Transistors",
                             "Integrated Circuits", "Timing", "Fuses", "Connectors", "smd widgets",
                             "Very long custom rack type name"}) {
      for (const auto standard : {SymbolStandard::Eu, SymbolStandard::Us}) {
        InventatoryRack rack;
        rack.code = "R99";
        rack.componentType = type;
        const auto zpl = service.buildRackLabelZpl(rack, standard);
        for (auto at = zpl.find("^FO"); at != string::npos; at = zpl.find("^FO", at + 3)) {
          const auto comma = zpl.find(',', at);
          const auto fieldX = stoi(zpl.substr(at + 3));
          const auto fieldY = stoi(zpl.substr(comma + 1));
          const auto end = zpl.find("^FS", at);
          const auto field = zpl.substr(at, end - at);
          assert(fieldX >= 7);
          if (const auto graphic = field.find("^GFA,"); graphic != string::npos) {
            const auto bytesPerRow = stoi(field.substr(field.find(',', field.find(',', graphic + 5) + 1) + 1));
            const auto bytes = stoi(field.substr(graphic + 5));
            assert(fieldX + bytesPerRow * 8 <= 243 && fieldY + bytes / bytesPerRow <= 179);
          } else if (const auto box = field.find("^GB"); box != string::npos) {
            const auto width = stoi(field.substr(box + 3));
            const auto height = stoi(field.substr(field.find(',', box) + 1));
            assert(fieldX + width <= 243 && fieldY + height <= 179);
          } else if (const auto font = field.find("^A0N,"); font != string::npos) {
            const auto size = stoi(field.substr(font + 5));
            const auto text = field.substr(field.find("^FD") + 3);
            assert(fieldX + estimateFont0Width(text, size, size) <= 243 && fieldY + size <= 179 + 4);
          }
        }
      }
    }

    // Symbol lookup follows the rack allocator's singular and plural spellings.
    assert(rackSymbolKey("Resistor") == "resistor" && rackSymbolKey("  RESISTORS ") == "resistor");
    assert(rackSymbolKey("Integrated Circuits") == "ic" && rackSymbolKey("ICs") == "ic");
    assert(rackSymbolKey("Timing") == "crystal" && rackSymbolKey("Crystals") == "crystal");
    assert(rackSymbolKey("Indicators") == "led" && rackSymbolKey("Fuses") == "fuse");
    assert(rackSymbolKey("") == "grid" && rackSymbolKey("Widgets") == "grid");
    for (size_t index = 0; index < kRackSymbolSetCount; ++index) {
      for (const auto* bitmap : {kRackSymbolSets[index].eu, kRackSymbolSets[index].us}) {
        assert(bitmap != nullptr && bitmap->width > 0 && bitmap->width <= 120 && bitmap->height > 0 &&
               bitmap->height <= 80 && bitmap->bytesPerRow == (bitmap->width + 7) / 8);
        bool anyBlack = false;
        for (int byte = 0; byte < bitmap->bytesPerRow * bitmap->height; ++byte) anyBlack = anyBlack || bitmap->data[byte] != 0;
        assert(anyBlack);
      }
    }
    // The connector's pin array sits centred in its outline, and the transistor is centred on its circle
    // rather than on a bounding box that also holds the long base lead.
    const auto symbolBitmap = [&](const string& key) -> const RackSymbolBitmap& {
      for (size_t index = 0; index < kRackSymbolSetCount; ++index) {
        if (key == kRackSymbolSets[index].key) return *kRackSymbolSets[index].eu;
      }
      assert(false);
      return *kRackSymbolSets[0].eu;
    };
    const auto mirrorMismatch = [](const RackSymbolBitmap& bitmap) {
      const auto black = [&](int x, int y) { return (bitmap.data[y * bitmap.bytesPerRow + x / 8] >> (7 - x % 8)) & 1; };
      int mismatched = 0;
      int total = 0;
      for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width / 2; ++x) {
          total += 1;
          mismatched += black(x, y) != black(bitmap.width - 1 - x, y) ? 1 : 0;
        }
      }
      return 100.0 * mismatched / total;
    };
    assert(mirrorMismatch(symbolBitmap("connector")) < 4.0);
    assert(mirrorMismatch(symbolBitmap("grid")) < 4.0);
    assert(abs(symbolBitmap("transistor").width - symbolBitmap("transistor").height) <= 1);

    SymbolStandard parsedStandard = SymbolStandard::Us;
    assert(parseSymbolStandard(" EU ", parsedStandard) && parsedStandard == SymbolStandard::Eu);
    assert(parseSymbolStandard("us", parsedStandard) && parsedStandard == SymbolStandard::Us);
    assert(!parseSymbolStandard("iso", parsedStandard) && parsedStandard == SymbolStandard::Us);

    error.clear();
    assert(service.printRackLabel(resistorRack, &error));
    assert(error.empty());
    assert(backendPtr->lastJobName_ == "Inventatory Rack R1");
    assert(backendPtr->lastZpl_.find("^FD01^FS") != string::npos);
    assert(backendPtr->lastZpl_.find("^GFA,510,510,15,") != string::npos);
    assert(service.printRackLabel(resistorRack, &error, SymbolStandard::Us));
    assert(backendPtr->lastZpl_.find("^GFA,630,630,15,") != string::npos);

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
    assert(tvsPlan.parameters.size() == 4);
    assert(tvsPlan.parameters[0].caption == "VST" && tvsPlan.parameters[0].value == "16V");
    assert(tvsPlan.parameters[1].caption == "VC" && tvsPlan.parameters[1].value == "26V");
    assert(tvsPlan.parameters[2].caption == "IPP" && tvsPlan.parameters[2].value == "23.1A");
    assert(labelTile(tvsPlan, "PPP") == "600W");
    const auto tvsZpl = service.buildZpl(tvsDiode);
    assert(tvsZpl.find("^FR^FDTVS DIODE^FS") != string::npos);
    assert(tvsZpl.find("^FO7,110^A0N,12,11^FDVST^FS") != string::npos);
    assert(tvsZpl.find("^FO7,123^A0N,22,22^FD16V^FS") != string::npos);
    assert(tvsZpl.find("^FO93,110^A0N,12,11^FDVC^FS") != string::npos);
    assert(tvsZpl.find("^FO7,149^A0N,12,11^FDIPP^FS") != string::npos);
    assert(tvsZpl.find("^FD23.1A^FS") != string::npos);
    assert(tvsZpl.find("^FO93,149^A0N,12,11^FDPPP^FS") != string::npos);
    assert(tvsZpl.find("^FO88,112^GB1,72,1^FS") != string::npos);

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
    assert(service.buildZpl(protectionIc).find("^FR^FDPROTECTION IC^FS") != string::npos);

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
    assert(service.buildZpl(opAmp).find("^FR^FDOP-AMP^FS") != string::npos);

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
    // The sensor type does not fit a tile whole, so it is left off rather than clipped.
    assert(imuPlan.parameters.size() == 3);
    assert(imuPlan.parameters[0].caption == "OUTPUT");
    assert(imuPlan.parameters[1].caption == "SUPPLY");
    assert(imuPlan.parameters[2].caption == "RES");
    assert(service.buildZpl(imu).find("^FR^FD3 AXIS IMU^FS") != string::npos);

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
      assert(service.buildZpl(buckFalsePositive).find("^FR^FDMEMORY IC^FS") != string::npos);
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
    assert(service.buildZpl(fallback).find("^FR^FDPROTOTYPE^FS") != string::npos);

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
    assert(inductorPlan.mainValue == "27nH");
    assert(inductorPlan.mainIsMeasuredValue);
    assert(labelTile(inductorPlan, "RATED") == "350mA");
    assert(labelTile(inductorPlan, "SRF") == "1.7GHz");
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
    assert(!plan.mainIsMeasuredValue);
    assert(plan.mainTolerance.empty());
    assert(plan.parameters.size() == 4);
    assert(plan.parameters[0].caption == "CORE" && plan.parameters[0].value == "Cortex-M0+");
    assert(plan.parameters[1].caption == "CLOCK" && plan.parameters[1].value == "64MHz");
    assert(plan.parameters[2].caption == "FLASH / RAM" && plan.parameters[2].value == "128KB/36KB");
    assert(plan.parameters[3].caption == "SUPPLY" && plan.parameters[3].value == "3.3V");
    const auto zpl = service.buildZpl(item);
    // A manufacturer that cannot fit beside the package pill uses its short name.
    assert(zpl.find("^FDSTMicro") == string::npos);
    assert(zpl.find("^FDST^FS") != string::npos);
    const auto pillField = zpl.find("^FR^FDLQFP-64^FS");
    assert(pillField != string::npos);
    // A long part name wraps onto two lines instead of being cut.
    const auto firstLine = zpl.find("^FO", zpl.find("^FX --- Main value ---"));
    assert(firstLine != string::npos);
    const auto firstText = zpl.find("^FD", firstLine) + 3;
    const auto secondLine = zpl.find("^FO", firstText);
    const auto secondText = zpl.find("^FD", secondLine) + 3;
    const auto wrapped = zpl.substr(firstText, zpl.find("^FS", firstText) - firstText) + " " +
                         zpl.substr(secondText, zpl.find("^FS", secondText) - secondText);
    assert(wrapped == "STM32G0 demo board");
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
    assert(plan.manufacturerLine == "Alpha & Omega");
    assert(plan.parameters.size() == 2);
    assert(labelTile(plan, "VDS") == "30V");
    assert(labelTile(plan, "ID") == "12A");

    const auto zpl = service.buildZpl(item);
    assert(zpl.find("^FR^FDN-MOSFET^FS") != string::npos);
    assert(zpl.find("^FDIRLML6344TRPBF^FS") != string::npos);
    assert(zpl.find("2W (Ta)") == string::npos);
    assert(zpl.find("TO-263-3,") == string::npos);
    const auto mainField = zpl.find("^FDIRLML6344TRPBF^FS");
    const auto mainFont = zpl.rfind("^A0N,", mainField);
    const auto mainSize = stoi(zpl.substr(mainFont + 5));
    assert(mainSize >= 18 && mainSize <= 40);
    assert(estimateFont0Width("IRLML6344TRPBF", mainSize, mainSize) <= 164);
    assert(zpl.find("^FO12,84^A0N,16,16^FR^FDTO-263-3^FS") != string::npos);
    assert(zpl.find("^FDVDS^FS") != string::npos);
    assert(zpl.find("^FD30V^FS") != string::npos);
    assert(zpl.find("^FDID^FS") != string::npos);
    assert(zpl.find("^FD12A^FS") != string::npos);
    // Two tiles fill one row, so the column rule stops after the first row.
    assert(zpl.find("^FO88,112^GB1,33,1^FS") != string::npos);
  }

  {
    LabelPrinterService service(make_unique<MockPrinterBackend>());
    InventoryItem longHeader;
    longHeader.partName = "Part number retained as the main value";
    longHeader.labelOverride = "Custom label beyond sixteen";
    const auto zpl = service.buildZpl(longHeader);
    assert(partShortDescription(longHeader) == "Custom label beyond sixteen");
    assert(zpl.find("^FR^FDCUSTOM LABEL BEYOND SIXTEEN^FS") != string::npos ||
           zpl.find("^FR^FDCUSTOM LABEL BEYOND") != string::npos);
    assert(zpl.find("^FO7,0^GB236,22,22,B,4^FS") != string::npos);
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
    assert(diodePlan.parameters.size() == 3);
    assert(diodePlan.parameters[0].caption == "VR" && diodePlan.parameters[0].value == "40V");
    assert(diodePlan.parameters[1].caption == "IO" && diodePlan.parameters[1].value == "1A");
    assert(diodePlan.parameters[2].caption == "VF" && diodePlan.parameters[2].value == "0.38V");

    InventoryItem connector;
    connector.id = "conn-1";
    connector.partName = "Board header";
    connector.manufacturer = "Harwin";
    connector.category = "Connectors";
    connector.parameters = {{"Pins", "8"}, {"Connector Type", "Header"}, {"Pitch", "2.54mm"}, {"Rows", "2"}};
    const auto connectorPlan = service.buildLabelPlan(connector);
    assert(connectorPlan.parameters.size() == 4);
    assert(labelTile(connectorPlan, "PINS") == "8");
    assert(labelTile(connectorPlan, "TYPE") == "Header");
    assert(labelTile(connectorPlan, "ROWS") == "2");
    assert(labelTile(connectorPlan, "PITCH") == "2.54mm");

    InventoryItem regulator;
    regulator.id = "reg-1";
    regulator.partName = "3.3V LDO";
    regulator.manufacturer = "Microchip";
    regulator.category = "Voltage Regulators";
    regulator.parameters = {{"Output Voltage", "3.3V"}, {"Voltage - Input", "5V"}, {"Output Current", "1A"}, {"Type", "LDO"}};
    const auto regulatorPlan = service.buildLabelPlan(regulator);
    assert(labelTile(regulatorPlan, "VOUT") == "3.3V");
    assert(labelTile(regulatorPlan, "VIN") == "5V");
    assert(labelTile(regulatorPlan, "IOUT") == "1A");
    assert(labelTile(regulatorPlan, "TYPE") == "LDO");

    InventoryItem crystal;
    crystal.id = "xtal-1";
    crystal.partName = "16MHz crystal";
    crystal.manufacturer = "Abracon";
    crystal.category = "Crystals";
    crystal.parameters = {{"Frequency", "16MHz"}, {"Load Capacitance", "18pF"}, {"ESR", "50Ohm"}};
    const auto crystalPlan = service.buildLabelPlan(crystal);
    assert(labelTile(crystalPlan, "FREQ") == "16MHz");
    assert(labelTile(crystalPlan, "LOAD C") == "18pF");
    assert(labelTile(crystalPlan, "ESR") == u8"50\u03A9");
  }

  {
    LabelPrinterService service(make_unique<MockPrinterBackend>());

    // DigiKey-style MLCC: value with tolerance as the main text, dielectric
    // and voltage as tiles, manufacturer (not the part number) under the value.
    InventoryItem mlcc;
    mlcc.id = "mlcc-1";
    mlcc.partName = "CAP CER 0.1UF 50V X7R 0402";
    mlcc.manufacturer = "Murata Electronics";
    mlcc.sku = "GRM155R71H104KE14D";
    mlcc.category = "Ceramic Capacitors";
    mlcc.machineCode = "14";
    mlcc.inventatoryId = "Inventatory:C-00014";
    mlcc.parameters = {{"Capacitance", "0.1 \u00B5F"},
                       {"Tolerance", "\u00B110%"},
                       {"Voltage - Rated", "50V"},
                       {"Temperature Coefficient", "X7R"},
                       {"Operating Temperature", "-55\u00B0C ~ 125\u00B0C"},
                       {"Package / Case", "0402 (1005 Metric)"},
                       {"Height - Seated (Max)", "-"},
                       {"Thickness (Max)", "0.022\" (0.55mm)"},
                       {"ESR (Equivalent Series Resistance)", "N/A"}};
    const auto mlccPlan = service.buildLabelPlan(mlcc, "R118-b2");
    assert(mlccPlan.mainIsMeasuredValue);
    assert(mlccPlan.mainValue == u8"0.1\u00B5F");
    assert(mlccPlan.mainTolerance == u8"\u00B110%");
    assert(mlccPlan.packageLine == "0402");
    assert(mlccPlan.manufacturerLine == "Murata Electronics");
    assert(mlccPlan.parameters.size() == 4);
    assert(labelTile(mlccPlan, "VOLTAGE") == "50V");
    assert(labelTile(mlccPlan, "DIELECTRIC") == "X7R");
    assert(labelTile(mlccPlan, "TEMP") == u8"-55 to 125\u00B0C");
    assert(labelTile(mlccPlan, "HEIGHT") == "0.55mm");
    assert(labelTile(mlccPlan, "ESR").empty());
    const auto mlccZpl = service.buildZpl(mlcc, "R118-b2");
    assert(mlccZpl.find("^FD-^FS") == string::npos);
    assert(mlccZpl.find("^FDN/A^FS") == string::npos);
    // Printers place labels about 10 dots low: nothing may extend below y 184,
    // and the QR symbol (drawn 10 dots below its origin) must clear the ID.
    for (const auto& zplLabel : {mlccZpl, service.buildZpl(mlcc)}) {
      for (auto at = zplLabel.find("^FO"); at != string::npos; at = zplLabel.find("^FO", at + 3)) {
        const auto comma = zplLabel.find(',', at);
        const auto fieldY = stoi(zplLabel.substr(comma + 1));
        const auto end = zplLabel.find("^FS", at);
        const auto field = zplLabel.substr(at, end - at);
        int fieldBottom = fieldY;
        if (const auto font = field.find("^A0N,"); font != string::npos) {
          fieldBottom = fieldY + stoi(field.substr(font + 5));
        } else if (const auto graphic = field.find("^GB"); graphic != string::npos) {
          fieldBottom = fieldY + stoi(field.substr(field.find(',', graphic) + 1));
        } else if (field.find("^BQN,2,3") != string::npos) {
          fieldBottom = fieldY + 10 + 21 * 3;
          const auto idField = zplLabel.find("^FDC-0014^FS");
          if (idField != string::npos) {
            const auto idOrigin = zplLabel.rfind("^FO", idField);
            assert(stoi(zplLabel.substr(zplLabel.find(',', idOrigin) + 1)) > fieldBottom);
          }
        }
        assert(fieldBottom <= 184);
      }
    }
    assert(mlccZpl.find("GRM155R71H104KE14D") == string::npos);
    assert(mlccZpl.find("^FDMurata Electronics^FS") != string::npos);
    assert(mlccZpl.find("^FD0.1\u00B5^FS") != string::npos);
    assert(mlccZpl.find("^FDF^FS") != string::npos);
    assert(mlccZpl.find("^FDC-0014^FS") != string::npos);
    assert(mlccZpl.find("^FDLA,0014^FS") != string::npos);
    assert(mlccZpl.find("^FR^FDR118^FS") != string::npos);
    assert(mlccZpl.find("^FDB2^FS") != string::npos);

    // Supplier package names win over case aliases; nonstandard packages
    // fall back to the metric size.
    InventoryItem mosfet;
    mosfet.category = "MOSFETs";
    mosfet.manufacturer = "Alpha & Omega Semiconductor Inc.";
    mosfet.sku = "AO3400A";
    mosfet.parameters = {{"Package / Case", "TO-236-3, SC-59, SOT-23-3"},
                         {"Supplier Device Package", "SOT-23-3L"},
                         {"Drain to Source Voltage (Vdss)", "30 V"},
                         {"Current - Continuous Drain (Id) @ 25\u00B0C", "5.7A (Ta)"},
                         {"Rds On (Max) @ Id, Vgs", "26.5mOhm @ 5.8A, 10V"},
                         {"Vgs(th) (Max) @ Id", "1.45V @ 250\u00B5A"},
                         {"Gate Charge (Qg) (Max) @ Vgs", "7 nC @ 4.5 V"}};
    const auto mosfetPlan = service.buildLabelPlan(mosfet);
    assert(mosfetPlan.mainValue == "AO3400A");
    assert(!mosfetPlan.mainIsMeasuredValue);
    assert(mosfetPlan.packageLine == "SOT-23-3L");
    assert(mosfetPlan.parameters.size() == 4);
    assert(labelTile(mosfetPlan, "VDS") == "30 V");
    assert(labelTile(mosfetPlan, "ID") == "5.7A");
    assert(labelTile(mosfetPlan, "RDS(ON)") == u8"26.5m\u03A9");
    assert(labelTile(mosfetPlan, "VGS(TH)") == "1.45V");
    assert(labelTile(mosfetPlan, "QG").empty());
    const auto mosfetZpl = service.buildZpl(mosfet);
    // A long manufacturer drops its corporate suffix instead of being cut.
    assert(mosfetZpl.find("^FDAlpha & Omega Semiconductor Inc.^FS") == string::npos);
    assert(mosfetZpl.find("^FDAlpha^FS") != string::npos);
    assert(mosfetZpl.find("Alpha &") == string::npos);

    InventoryItem inductor;
    inductor.category = "Fixed Inductors";
    inductor.manufacturer = "Bourns Inc.";
    inductor.parameters = {{"Inductance", "4.7 \u00B5H"},
                           {"Tolerance", "\u00B120%"},
                           {"Current Rating (Amps)", "3A"},
                           {"Current - Saturation (Isat)", "3.6A"},
                           {"DC Resistance (DCR)", "48mOhm Max"},
                           {"Shielding", "Shielded"},
                           {"Package / Case", "Nonstandard"},
                           {"Size / Dimension", "0.157\" L x 0.157\" W (4.00mm x 4.00mm)"}};
    const auto inductorPlan = service.buildLabelPlan(inductor);
    assert(inductorPlan.mainValue == u8"4.7\u00B5H");
    assert(inductorPlan.packageLine == "4x4mm");
    assert(labelTile(inductorPlan, "RATED") == "3A");
    assert(labelTile(inductorPlan, "ISAT") == "3.6A");
    assert(labelTile(inductorPlan, "DCR") == u8"48m\u03A9");
    assert(labelTile(inductorPlan, "SHIELD") == "Shielded");

    // ICs print the manufacturer part number rather than the distributor description.
    InventoryItem mcu;
    mcu.category = "Microcontrollers";
    mcu.partName = "IC MCU 32BIT 64KB FLASH 32LQFP";
    mcu.sku = "STM32G031K8T6";
    mcu.parameters = {{"Program Memory Size", "64KB (64K x 8)"}, {"RAM Size", "8K x 8"}};
    const auto mcuPlan = service.buildLabelPlan(mcu);
    assert(mcuPlan.mainValue == "STM32G031K8T6");
    assert(labelTile(mcuPlan, "FLASH / RAM") == "64KB/8K");

    // Parts without a manufacturer leave the line empty instead of repeating the name.
    InventoryItem anonymous;
    anonymous.partName = "Mystery board";
    assert(service.buildLabelPlan(anonymous).manufacturerLine.empty());

    // Tile values drop vendor qualifiers and marks.
    assert(labelTileValue("0.1W, 1/10W") == "0.1W");
    assert(labelTileValue(u8"ARM\u00AE Cortex\u00AE-M0+") == "Cortex-M0+");
    assert(labelTileValue("1.7V ~ 3.6V") == "1.7 to 3.6V");
    assert(labelTileValue("2.7V ~ 5.5V") == "2.7 to 5.5V");
    assert(labelTileValue("8 ~ 12") == "8 to 12");
    assert(labelTileValue("500 mV @ 3 A") == "500 mV");

    // Width fitting steps down, then shortens; nothing exceeds the box.
    const auto fits = fitFont0Text("SS34", 166, {40, 36});
    assert(fits.size == 40 && fits.text == "SS34");
    const auto steps = fitFont0Text("STM32G031K8T6", 166, {40, 36, 32, 28, 26, 24, 22});
    assert(steps.size < 40 && steps.text == "STM32G031K8T6" && steps.width <= 166);
    const auto shortened = fitFont0Text("A very long manufacturer name that cannot fit", 60, {16, 12});
    assert(shortened.size == 12);
    assert(shortened.width <= 60);
    assert(shortened.text.size() > 3 && shortened.text.compare(shortened.text.size() - 3, 3, "...") == 0);
    const auto utf8 = fitFont0Text(u8"\u03A9\u03A9\u03A9\u03A9\u03A9\u03A9\u03A9\u03A9", 20, {12});
    assert(utf8.width <= 20);
    assert(utf8.text.find(u8"\u03A9") == 0 || utf8.text == "...");
    assert(fitFont0Text("", 100, {12}).text.empty());
  }

  {
    LabelPrinterService service(make_unique<MockPrinterBackend>());

    // The photographed label: "Mounting Type" must never reach the dielectric
    // tile, and electrolytics (which have no dielectric parameter) simply
    // have no such tile.
    InventoryItem electrolytic;
    electrolytic.id = "cap-electrolytic";
    electrolytic.category = "Aluminum Electrolytic Capacitors";
    electrolytic.manufacturer = "Panasonic Industry";
    electrolytic.inventatoryId = "Inventatory:C-00007";
    electrolytic.machineCode = "0007";
    electrolytic.parameters = {{"Capacitance", u8"100 \u00B5F"},
                               {"Tolerance", u8"\u00B120%"},
                               {"Voltage - Rated", "16 V"},
                               {"Operating Temperature", u8"-40\u00B0C ~ 85\u00B0C"},
                               {"Mounting Type", "Through Hole"},
                               {"Package / Case", "Radial, Can"},
                               {"Type", "Through Hole"},
                               {"Height - Seated (Max)", "0.472\" (12.00mm)"}};
    const auto electrolyticPlan = service.buildLabelPlan(electrolytic);
    assert(labelTile(electrolyticPlan, "DIELECTRIC").empty());
    assert(labelTile(electrolyticPlan, "VOLTAGE") == "16 V");
    assert(labelTile(electrolyticPlan, "TEMP") == u8"-40 to 85\u00B0C");
    assert(labelTile(electrolyticPlan, "HEIGHT") == "12mm");
    assert(electrolyticPlan.parameters.size() == 3);
    const auto electrolyticZpl = service.buildZpl(electrolytic);
    assert(electrolyticZpl.find("Through") == string::npos);
    assert(electrolyticZpl.find("...") == string::npos);
    assert(electrolyticZpl.find("..") == string::npos);
    // The tolerance sign sits 3 dots above its digits' baseline offset.
    const auto signField = electrolyticZpl.find(u8"^FD\u00B1^FS");
    const auto percentField = electrolyticZpl.find("^FD20%^FS");
    assert(signField != string::npos && percentField != string::npos);
    const auto fieldY = [&](size_t field) {
      const auto origin = electrolyticZpl.rfind("^FO", field);
      return stoi(electrolyticZpl.substr(electrolyticZpl.find(',', origin) + 1));
    };
    assert(fieldY(signField) == fieldY(percentField) - 3);

    // Film capacitors name their material instead of a coefficient.
    InventoryItem film;
    film.category = "Film Capacitors";
    film.parameters = {{"Capacitance", "0.1 \u00B5F"},
                       {"Dielectric Material", "Polypropylene (PP), Metallized"},
                       {"Mounting Type", "Through Hole"}};
    assert(labelTile(service.buildLabelPlan(film), "DIELECTRIC") == "PP");

    // A value without the unit its caption promises is a mismatched parameter.
    InventoryItem mismatched;
    mismatched.category = "Capacitors";
    mismatched.parameters = {{"Capacitance", "1 nF"},
                             {"Voltage - Rated", "Polar"},
                             {"Operating Temperature", "Industrial"},
                             {"Height - Seated (Max)", "Tall"},
                             {"ESR (Equivalent Series Resistance)", "50 mOhm @ 100kHz"}};
    const auto mismatchedPlan = service.buildLabelPlan(mismatched);
    assert(labelTile(mismatchedPlan, "VOLTAGE").empty());
    assert(labelTile(mismatchedPlan, "TEMP").empty());
    assert(labelTile(mismatchedPlan, "HEIGHT").empty());
    assert(labelTile(mismatchedPlan, "ESR") == u8"50 m\u03A9");

    // Short names match whole: "Type" never picks up "Mounting Type".
    InventoryItem regulator;
    regulator.category = "Voltage Regulators";
    regulator.parameters = {{"Mounting Type", "Surface Mount"}, {"Output Type", "Fixed"}};
    const auto regulatorPlan = service.buildLabelPlan(regulator);
    assert(labelTile(regulatorPlan, "TYPE") == "Fixed");
    regulator.parameters = {{"Mounting Type", "Surface Mount"}};
    assert(labelTile(service.buildLabelPlan(regulator), "TYPE").empty());

    // A tile value is printed whole or not at all, and ranges keep their
    // meaning: a thousands separator is not a list separator.
    InventoryItem sensor;
    sensor.category = "Sensors";
    sensor.parameters = {{"Output Type", "Pulse Width Modulation, 12-bit"}, {"Resolution", "1,024 steps"}};
    const auto sensorPlan = service.buildLabelPlan(sensor);
    assert(labelTile(sensorPlan, "OUTPUT").empty());
    assert(labelTile(sensorPlan, "RES") == "1,024 steps");
    assert(labelTileValue("Polyester, Metallized") == "Polyester");
    assert(labelTileValue("0.472\" (12.50mm)") == "12.5mm");
    assert(labelTileValue(u8"-55\u00B0C ~ 150\u00B0C (TJ)") == u8"-55 to 150\u00B0C");
  }

  {
    LabelPrinterService service(make_unique<MockPrinterBackend>());

    // Real DigiKey scans file diodes and MOSFETs under "Discrete Semiconductor
    // Products"; their kind comes from the parameters.
    InventoryItem schottky;
    schottky.partName = "DIODE SCHOTTKY 60V 5A DO214AB";
    schottky.manufacturer = "Comchip Technology";
    schottky.category = "Discrete Semiconductor Products";
    schottky.sku = "641-1127-1-ND";
    schottky.digikeyPartNumber = "641-1127-1-ND";
    schottky.vendorMetadata.manufacturerPartNumber = "CDBC560-G";
    schottky.machineCode = "0023";
    schottky.parameters = {{"Technology", "Schottky"},
                           {"Voltage - DC Reverse (Vr) (Max)", "60 V"},
                           {"Current - Average Rectified (Io)", "5A"},
                           {"Voltage - Forward (Vf) (Max) @ If", "750 mV @ 5 A"},
                           {"Speed", "Fast Recovery =< 500ns, > 200mA (Io)"},
                           {"Mounting Type", "Surface Mount"},
                           {"Package / Case", "DO-214AB, SMC"},
                           {"Supplier Device Package", "DO-214AB (SMC)"}};
    const auto schottkyPlan = service.buildLabelPlan(schottky, "R3-A3");
    assert(schottkyPlan.mainValue == "CDBC560-G");
    assert(labelTile(schottkyPlan, "VR") == "60 V");
    assert(labelTile(schottkyPlan, "IO") == "5A");
    assert(labelTile(schottkyPlan, "VF") == "750 mV");
    assert(labelTile(schottkyPlan, "TECH") == "Schottky");
    const auto schottkyZpl = service.buildZpl(schottky, "R3-A3");
    assert(schottkyZpl.find("641-1127") == string::npos);
    assert(schottkyZpl.find("^FDComchip^FS") != string::npos);

    InventoryItem fet;
    fet.partName = "MOSFET N-CH 30V 5A MICRO3/SOT23";
    fet.manufacturer = "Infineon Technologies";
    fet.category = "Discrete Semiconductor Products";
    fet.sku = "IRLML6344TRPBF";
    fet.vendorMetadata.manufacturerPartNumber = "IRLML6344TRPBF";
    fet.parameters = {{"FET Type", "N-Channel"},
                      {"Drain to Source Voltage (Vdss)", "30 V"},
                      {"Current - Continuous Drain (Id) @ 25\u00B0C", "5A (Ta)"},
                      {"Rds On (Max) @ Id, Vgs", "29mOhm @ 5A, 4.5V"},
                      {"Vgs(th) (Max) @ Id", u8"1.1V @ 10\u00B5A"},
                      {"Gate Charge (Qg) (Max) @ Vgs", "6.8 nC @ 4.5 V"},
                      {"Supplier Device Package", u8"Micro3\u2122/SOT-23"},
                      {"Mounting Type", "Surface Mount"}};
    const auto fetPlan = service.buildLabelPlan(fet, "R6-A2");
    assert(fetPlan.parameters.size() == 4);
    assert(labelTile(fetPlan, "VDS") == "30 V");
    assert(labelTile(fetPlan, "ID") == "5A");
    assert(labelTile(fetPlan, "RDS(ON)") == u8"29m\u03A9");
    assert(labelTile(fetPlan, "VGS(TH)") == "1.1V");
    assert(fetPlan.packageLine == "Micro3");
    assert(service.buildZpl(fet, "R6-A2").find("^FDInfineon^FS") != string::npos);

    // Candidates run from the full name to the shortest.
    const auto names = label_printer_detail::manufacturerCandidates("Infineon Technologies");
    assert(names.size() == 2 && names[0] == "Infineon Technologies" && names[1] == "Infineon");
    assert(label_printer_detail::manufacturerCandidates("Nexperia USA Inc.").back() == "Nexperia");
    assert(label_printer_detail::manufacturerCandidates("Murata").size() == 1);
    const auto alpha = label_printer_detail::manufacturerCandidates("Alpha & Omega Semiconductor Inc.");
    assert(alpha.size() == 4 && alpha[2] == "Alpha & Omega" && alpha[3] == "Alpha");
    const auto shenzhen = label_printer_detail::manufacturerCandidates("Shenzhen Slkormicro Semicon Co., Ltd.");
    assert(shenzhen.back() == "Slkormicro");
    assert(label_printer_detail::manufacturerCandidates("Texas Instruments").back() == "TI");

    // A DigiKey buck converter mentions "Synchronous Rectifier" and a TVS is
    // named "DIODE": neither may be classified as an ordinary diode.
    InventoryItem buck;
    buck.partName = "IC REG BUCK 3.3V 2A TSOT23-6";
    buck.category = "Integrated Circuits (ICs)";
    buck.parameters = {{"Topology", "Buck"},
                       {"Output Type", "Fixed"},
                       {"Voltage - Input (Max)", "32V"},
                       {"Voltage - Output (Min/Fixed)", "3.3V"},
                       {"Current - Output", "2A"},
                       {"Synchronous Rectifier", "Yes"},
                       {"Type", "Surface Mount"}};
    const auto buckPlan = service.buildLabelPlan(buck);
    assert(labelTile(buckPlan, "VOUT") == "3.3V");
    assert(labelTile(buckPlan, "VIN") == "32V");
    assert(labelTile(buckPlan, "IOUT") == "2A");
    assert(labelTile(buckPlan, "VR").empty());

    InventoryItem smdFuse;
    smdFuse.partName = "FUSE BRD MT 500MA 125VAC 63VDC";
    smdFuse.category = "Circuit Protection";
    smdFuse.parameters = {{"Fuse Type", "Board Mount"},
                          {"Current Rating (Amps)", "500 mA"},
                          {"Voltage Rating - AC", "125 V"},
                          {"Voltage Rating - DC", "63 V"},
                          {"Response Time", "Fast Blow"}};
    const auto fusePlan = service.buildLabelPlan(smdFuse);
    assert(fusePlan.parameters.size() == 4);
    assert(labelTile(fusePlan, "VDC") == "63 V" && labelTile(fusePlan, "SPEED") == "Fast Blow");

    InventoryItem timer;
    timer.partName = "IC OSC SNGL TIMER 100KHZ 8-SOIC";
    timer.category = "Integrated Circuits (ICs)";
    timer.parameters = {{"Type", "Surface Mount"},
                        {"Frequency", "100kHz"},
                        {"Voltage - Supply", "4.5V ~ 16V"},
                        {"Operating Temperature", u8"0\u00B0C ~ 70\u00B0C"}};
    const auto timerPlan = service.buildLabelPlan(timer);
    assert(labelTile(timerPlan, "SUPPLY") == "4.5 to 16V");
    assert(labelTile(timerPlan, "FREQ") == "100kHz");
    assert(labelTile(timerPlan, "VR").empty());
    // A hyphen prints wider than a digit; the estimate must not under-count it.
    assert(estimateFont0Width("641-1127-1-ND", 22, 22) > estimateFont0Width("641112711ND", 22, 22) + 20);
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
    // Printer configuration is replaced only after the complete temporary
    // file has been written and flushed. A failed replacement must not damage
    // the existing path, and malformed/oversized files must not be accepted.
#ifdef _WIN32
    const auto directory = filesystem::temp_directory_path() / L"inventatory-printer-\u017c\u00f3\u0142\u0107";
#else
    const auto directory = filesystem::temp_directory_path() / "inventatory-printer-config-test";
#endif
    error_code cleanupError;
    filesystem::remove_all(directory, cleanupError);

    const auto configPath = directory / "printer.conf";
    const string configuredName = "Zebra " + string("\xCE\xBB") + " label printer";
    LabelPrinterService service(make_unique<MockPrinterBackend>());
    service.setConfiguredPrinter(configuredName);
    assert(service.saveConfig(configPath));

    LabelPrinterService loaded(make_unique<MockPrinterBackend>());
    assert(loaded.loadConfig(configPath));
    assert(loaded.configuredPrinter() == configuredName);

    {
      ofstream malformed(configPath, ios::binary | ios::trunc);
      assert(malformed);
      malformed << quoted(configuredName) << " trailing-data\n";
      malformed.flush();
      assert(malformed);
    }
    LabelPrinterService malformedService(make_unique<MockPrinterBackend>());
    assert(!malformedService.loadConfig(configPath));
    assert(!malformedService.hasConfiguredPrinter());

    {
      ofstream oversized(configPath, ios::binary | ios::trunc);
      assert(oversized);
      oversized << '"' << string(5000, 'x') << "\"\n";
      oversized.flush();
      assert(oversized);
    }
    assert(!malformedService.loadConfig(configPath));

    // A directory at the destination makes the final atomic replacement fail
    // after the temporary file has been written. It must remain intact.
    const auto replacementFailurePath = directory / "existing-destination";
    assert(filesystem::create_directory(replacementFailurePath, cleanupError));
    assert(!service.saveConfig(replacementFailurePath));
    assert(filesystem::is_directory(replacementFailurePath, cleanupError));
    assert(!cleanupError);

    // An unconfigured service still writes the valid empty configuration used
    // by the normal first-run save path.
    const auto emptyConfigPath = directory / "empty.conf";
    LabelPrinterService empty(make_unique<MockPrinterBackend>());
    assert(empty.saveConfig(emptyConfigPath));
    ifstream emptyConfig(emptyConfigPath, ios::binary);
    string emptyContents((istreambuf_iterator<char>(emptyConfig)), istreambuf_iterator<char>());
    assert(emptyContents == "\"\"\n");
    emptyConfig.close();

    filesystem::remove_all(directory, cleanupError);
    assert(!cleanupError);
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

    // Workspace-bound replay files must not share counters. A fresh workspace
    // with a newly scoped token rejects the previous workspace's token and can
    // start its own counter sequence at one.
    const auto workspaceA = stateDirectory / "workspace-a";
    const auto workspaceB = stateDirectory / "workspace-b";
    const auto replayA = inventatoryScanReplayStatePath(workspaceA);
    const auto replayB = inventatoryScanReplayStatePath(workspaceB);
    const string workspaceBToken = "222202030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    atomic<int> isolatedSyncCalls{0};
    auto isolatedOnSync = [&isolatedSyncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response,
                                               string&) {
      ++isolatedSyncCalls;
      response.requestId = request.requestId;
      return true;
    };
    LocalHttpServer workspaceServerA;
    workspaceServerA.setDeviceCredentials(deviceId, token, replayA);
    assert(workspaceServerA.start(19420, isolatedOnSync));
    const auto workspaceAResponse = sendLocalHttpRequest(
        workspaceServerA.port(), signedSyncRequest(token, deviceId, 7, body));
    assert(workspaceAResponse.rfind("HTTP/1.1 200 OK", 0) == 0);
    workspaceServerA.stop();
    assert(filesystem::exists(replayA));

    LocalHttpServer workspaceServerB;
    workspaceServerB.setDeviceCredentials(deviceId, workspaceBToken, replayB);
    assert(workspaceServerB.start(19421, isolatedOnSync));
    const auto oldWorkspaceRequest = sendLocalHttpRequest(
        workspaceServerB.port(), signedSyncRequest(token, deviceId, 8, body));
    assert(oldWorkspaceRequest.rfind("HTTP/1.1 401 Unauthorized", 0) == 0);
    const auto workspaceBResponse = sendLocalHttpRequest(
        workspaceServerB.port(), signedSyncRequest(workspaceBToken, deviceId, 1, body));
    assert(workspaceBResponse.rfind("HTTP/1.1 200 OK", 0) == 0);
    workspaceServerB.stop();
    assert(filesystem::exists(replayB));
    assert(isolatedSyncCalls == 2);

    LocalHttpServer server;
    server.setDeviceCredentials(deviceId, token, replayState);
    assert(server.start(19430, onSync));
    const auto boundAddresses = server.addresses();
    const auto privateAddresses = privateLocalAddresses();
    assert(boundAddresses.size() == 1);
    assert(boundAddresses.front() == (privateAddresses.empty() ? "127.0.0.1" : privateAddresses.front()));
    assert(boundAddresses.front() != "0.0.0.0");
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

    auto duplicateLength = firstRequest;
    const auto lengthEnd = duplicateLength.find("\r\n", duplicateLength.find("Content-Length:"));
    assert(lengthEnd != string::npos);
    duplicateLength.insert(lengthEnd + 2, "Content-Length: " + to_string(body.size()) + "\r\n");
    const auto duplicateLengthResponse = sendLocalHttpRequest(server.port(), duplicateLength);
    assert(duplicateLengthResponse.rfind("HTTP/1.1 400 Bad Request", 0) == 0);
    assert(syncCalls == 1);

    const auto replayed = sendLocalHttpRequest(server.port(), firstRequest);
    assert(replayed.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 1);

    vector<NativeSocket> slowClients;
    for (int index = 0; index < 4; ++index) slowClients.push_back(connectSlowLocalClient(server.port()));
    this_thread::sleep_for(chrono::milliseconds(100));
    const auto before = chrono::steady_clock::now();
    const auto nextResponse = sendLocalHttpRequest(server.port(), signedSyncRequest(token, deviceId, 43, body));
    const auto elapsed = chrono::steady_clock::now() - before;
    for (const auto client : slowClients) closeSocket(client);
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

#ifndef _WIN32
    {
      // Linux: the listener and accepted connections must be close-on-exec so a helper process
      // spawned while the service runs (xdg-open and the browser it starts) cannot keep the
      // scanner port bound after the service stops or restarts.
      const auto cloexecSocketsOnPort = [](uint16_t port, size_t& found) {
        bool allCloexec = true;
        found = 0;
        for (const auto& entry : filesystem::directory_iterator("/proc/self/fd")) {
          const int descriptor = atoi(entry.path().filename().string().c_str());
          sockaddr_in local{};
          socklen_t localSize = sizeof(local);
          if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 ||
              local.sin_family != AF_INET || ntohs(local.sin_port) != port) continue;
          ++found;
          const int flags = fcntl(descriptor, F_GETFD);
          if (flags < 0 || (flags & FD_CLOEXEC) == 0) allCloexec = false;
        }
        return allCloexec;
      };

      LocalHttpServer inheritServer;
      inheritServer.setDeviceCredentials(deviceId, rotatedToken, replayState);
      assert(inheritServer.start(19490, onSync));
      const uint16_t inheritPort = inheritServer.port();
      assert(inheritPort == 19490);
      const NativeSocket inheritClient = connectSlowLocalClient(inheritPort);
      this_thread::sleep_for(chrono::milliseconds(100));
      size_t socketsOnPort = 0;
      assert(cloexecSocketsOnPort(inheritPort, socketsOnPort));
      assert(socketsOnPort >= 1);

      inheritServer.stop();
      closeSocket(inheritClient);

      // A previous application instance serves, spawns a long-lived helper (as opening a link does)
      // and exits without a clean stop(): the helper must not keep the port in LISTEN.
      int execReady[2]{-1, -1};
      int helperReport[2]{-1, -1};
      assert(pipe2(execReady, O_CLOEXEC) == 0);
      assert(pipe2(helperReport, O_CLOEXEC) == 0);
      const pid_t previousInstance = fork();
      assert(previousInstance >= 0);
      if (previousInstance == 0) {
        LocalHttpServer previous;
        previous.setDeviceCredentials(deviceId, rotatedToken, replayState);
        if (!previous.start(19490, onSync)) _exit(2);
        const pid_t helper = fork();
        if (helper < 0) _exit(3);
        if (helper == 0) {
          execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
          _exit(127);
        }
        close(execReady[1]);
        char readyByte = 0;
        // EOF arrives only once the helper has exec'd (or died): its inherited copies are final.
        while (read(execReady[0], &readyByte, 1) < 0 && errno == EINTR) {}
        if (write(helperReport[1], &helper, sizeof(helper)) != static_cast<ssize_t>(sizeof(helper))) _exit(4);
        _exit(0);
      }
      close(execReady[0]);
      close(execReady[1]);
      close(helperReport[1]);
      pid_t helperPid = -1;
      const ssize_t reported = read(helperReport[0], &helperPid, sizeof(helperPid));
      close(helperReport[0]);
      int previousStatus = 0;
      assert(waitpid(previousInstance, &previousStatus, 0) == previousInstance);
      assert(reported == static_cast<ssize_t>(sizeof(helperPid)));
      assert(WIFEXITED(previousStatus) && WEXITSTATUS(previousStatus) == 0);

      LocalHttpServer rebound;
      rebound.setDeviceCredentials(deviceId, rotatedToken, replayState);
      const bool reboundStarted = rebound.start(19490, onSync);
      const uint16_t reboundPort = reboundStarted ? rebound.port() : 0;
      rebound.stop();
      kill(helperPid, SIGKILL);
      waitpid(helperPid, nullptr, 0);
      assert(reboundStarted);
      assert(reboundPort == 19490);
    }
#endif

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

    const auto rotationReplayState = stateDirectory / "rotation-replay.state";
    atomic<bool> rotationCallbackEntered{false};
    atomic<bool> releaseRotationCallback{false};
    atomic<bool> rotationFinished{false};
    string rotationResponse;
    auto rotationOnSync = [&](const DeviceSyncRequest& request, DeviceSyncResponse& response, string&) {
      rotationCallbackEntered.store(true);
      while (!releaseRotationCallback.load()) this_thread::sleep_for(chrono::milliseconds(1));
      response.requestId = request.requestId;
      return true;
    };
    LocalHttpServer rotationServer;
    rotationServer.setDeviceCredentials(deviceId, token, rotationReplayState);
    assert(rotationServer.start(19461, rotationOnSync));
    const auto rotationRequest = signedSyncRequest(token, deviceId, 200, body);
    thread rotationRequestThread([&] { rotationResponse = sendLocalHttpRequest(rotationServer.port(), rotationRequest); });
    for (int attempt = 0; attempt < 100 && !rotationCallbackEntered.load(); ++attempt) {
      this_thread::sleep_for(chrono::milliseconds(5));
    }
    assert(rotationCallbackEntered.load());
    thread credentialRotationThread([&] {
      rotationServer.setDeviceCredentials(deviceId, rotatedToken, rotationReplayState);
      rotationFinished.store(true);
    });
    for (int attempt = 0; attempt < 100 && !rotationFinished.load(); ++attempt) {
      this_thread::sleep_for(chrono::milliseconds(5));
    }
    // Rotation must not wait for a callback that may itself be waiting for
    // foreground/UI work. The in-flight request is invalidated by the epoch.
    assert(rotationFinished.load());
    releaseRotationCallback.store(true);
    rotationRequestThread.join();
    credentialRotationThread.join();
    assert(rotationResponse.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    rotationServer.stop();

    const auto retryReplayState = stateDirectory / "retry-replay.state";
    atomic<int> retrySyncCalls{0};
    auto retryOnSync = [&retrySyncCalls](const DeviceSyncRequest& request, DeviceSyncResponse& response,
                                         string& error) {
      if (++retrySyncCalls == 1) {
        error.assign(9000, 'E');
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
    assert(failedSync.size() < 5000);
    assert(failedSync.find(string(257, 'E')) == string::npos);
    const auto retriedSync =
        sendLocalHttpRequest(retryServer.port(), signedSyncRequest(token, deviceId, 101, body));
    assert(retriedSync.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(retrySyncCalls == 2);
    retryServer.stop();

    const auto overflowReplayState = stateDirectory / "overflow-replay.state";
    atomic<int> overflowSyncCalls{0};
    auto overflowOnSync = [&overflowSyncCalls](const DeviceSyncRequest&, DeviceSyncResponse& response, string&) {
      ++overflowSyncCalls;
      response.requestId = string(kInventatoryScanResponseBodyLimit + 1U, 'X');
      return true;
    };
    LocalHttpServer overflowServer;
    overflowServer.setDeviceCredentials(deviceId, token, overflowReplayState);
    assert(overflowServer.start(19482, overflowOnSync));
    const auto oversizedResponse =
        sendLocalHttpRequest(overflowServer.port(), signedSyncRequest(token, deviceId, 300, body));
    assert(oversizedResponse.rfind("HTTP/1.1 500 Internal Server Error", 0) == 0);
    assert(oversizedResponse.find("Device sync response exceeds the scanner limit") != string::npos);
    const auto oversizedResponseReplay =
        sendLocalHttpRequest(overflowServer.port(), signedSyncRequest(token, deviceId, 300, body));
    assert(oversizedResponseReplay.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(overflowSyncCalls == 1);
    overflowServer.stop();

    // A replay-marker write failure must fail closed after the durable
    // callback, and the same counter must not invoke the callback again.
    const auto replayFailureParent = stateDirectory / "replay-failure-parent";
    {
      ofstream parentFile(replayFailureParent, ios::binary | ios::trunc);
      parentFile << "not a directory";
    }
    atomic<int> replayFailureSyncCalls{0};
    auto replayFailureOnSync = [&replayFailureSyncCalls](const DeviceSyncRequest& request,
                                                         DeviceSyncResponse& response, string&) {
      ++replayFailureSyncCalls;
      response.requestId = request.requestId;
      return true;
    };
    LocalHttpServer replayFailureServer;
    replayFailureServer.setDeviceCredentials(deviceId, token, replayFailureParent / "replay.state");
    assert(replayFailureServer.start(19481, replayFailureOnSync));
    const auto markerFailure = sendLocalHttpRequest(
        replayFailureServer.port(), signedSyncRequest(token, deviceId, 201, body));
    assert(markerFailure.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(replayFailureSyncCalls == 1);
    const auto markerFailureReplay = sendLocalHttpRequest(
        replayFailureServer.port(), signedSyncRequest(token, deviceId, 201, body));
    assert(markerFailureReplay.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(replayFailureSyncCalls == 1);
    replayFailureServer.stop();

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

    const auto malformedFingerprintState = stateDirectory / "malformed-fingerprint.state";
    {
      ofstream malformed(malformedFingerprintState, ios::trunc);
      malformed << "fingerprint=short\n"
                << "counter=1\n";
    }
    LocalHttpServer malformedFingerprintServer;
    malformedFingerprintServer.setDeviceCredentials(deviceId, rotatedToken, malformedFingerprintState);
    assert(malformedFingerprintServer.start(19471, onSync));
    const auto rejectedWithMalformedFingerprint = sendLocalHttpRequest(
        malformedFingerprintServer.port(), signedSyncRequest(rotatedToken, deviceId, 1, body));
    assert(rejectedWithMalformedFingerprint.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 3);
    malformedFingerprintServer.stop();

    // State left behind by a previous pairing secret (a rotation that crashed before the file was
    // removed) is for a different key: the new key starts a fresh sequence instead of being locked out,
    // and the first accepted request rewrites the file for the current key.
    const auto foreignReplayState = stateDirectory / "foreign-replay.state";
    {
      ofstream foreign(foreignReplayState, ios::trunc);
      foreign << "fingerprint=" << deviceTransportStateFingerprint(token) << '\n' << "counter=50\n";
    }
    LocalHttpServer foreignStateServer;
    foreignStateServer.setDeviceCredentials(deviceId, rotatedToken, foreignReplayState);
    assert(foreignStateServer.start(19473, onSync));
    const auto acceptedWithForeignState =
        sendLocalHttpRequest(foreignStateServer.port(), signedSyncRequest(rotatedToken, deviceId, 1, body));
    assert(acceptedWithForeignState.rfind("HTTP/1.1 200 OK", 0) == 0);
    assert(syncCalls == 4);
    {
      ifstream rewritten(foreignReplayState);
      stringstream rewrittenText;
      rewrittenText << rewritten.rdbuf();
      assert(rewrittenText.str() ==
             "fingerprint=" + deviceTransportStateFingerprint(rotatedToken) + "\ncounter=1\n");
    }
    const auto replayedAfterForeignState =
        sendLocalHttpRequest(foreignStateServer.port(), signedSyncRequest(rotatedToken, deviceId, 1, body));
    assert(replayedAfterForeignState.rfind("HTTP/1.1 409 Conflict", 0) == 0);
    assert(syncCalls == 4);
    foreignStateServer.stop();

    LocalHttpServer queuedStopServer;
    queuedStopServer.setDeviceCredentials(deviceId, rotatedToken, stateDirectory / "queued-stop.state");
    assert(queuedStopServer.start(19472, onSync));
    vector<NativeSocket> queuedSlowClients;
    for (int index = 0; index < 20; ++index) queuedSlowClients.push_back(connectSlowLocalClient(queuedStopServer.port()));
    this_thread::sleep_for(chrono::milliseconds(100));
    const auto stopStarted = chrono::steady_clock::now();
    queuedStopServer.stop();
    const auto stopElapsed = chrono::steady_clock::now() - stopStarted;
    for (const auto client : queuedSlowClients) closeSocket(client);
    assert(stopElapsed < chrono::seconds(4));

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
    InventoryItem secondItem = item;
    secondItem.id = "scan-r1-second-item";
    secondItem.machineCode = "0003";
    secondItem.partName = "1k resistor";
    secondItem.quantity = 7;
    store.items().push_back(secondItem);

    DeviceQuantityRequest request{"r1-a", "req-9", "0002", -2};
    unordered_map<string, DeviceQuantityCacheEntry> cache;
    deque<string> order;
    const auto first = applyDeviceQuantityCached(store, request, 1, cache, order);
    const auto second = applyDeviceQuantityCached(store, request, 1, cache, order);
    assert(first.ok);
    assert(second.ok);
    assert(first.appliedDelta == -2);
    assert(second.appliedDelta == -2);
    assert(store.items().front().quantity == 3);

    auto changedDelta = request;
    changedDelta.delta = -1;
    const auto deltaResult = applyDeviceQuantityCached(store, changedDelta, 1, cache, order);
    assert(deltaResult.ok);
    assert(deltaResult.appliedDelta == -1);
    assert(store.items().front().quantity == 2);

    auto changedDevice = changedDelta;
    changedDevice.deviceId = "r1-b";
    const auto deviceResult = applyDeviceQuantityCached(store, changedDevice, 1, cache, order);
    assert(deviceResult.ok);
    assert(deviceResult.appliedDelta == -1);
    assert(store.items().front().quantity == 1);

    auto changedCode = changedDevice;
    changedCode.code = "0003";
    const auto codeResult = applyDeviceQuantityCached(store, changedCode, 1, cache, order);
    assert(codeResult.ok);
    assert(codeResult.item == "1k resistor");
    assert(codeResult.appliedDelta == -1);
    assert(store.items().back().quantity == 6);

    auto changedWorkspace = request;
    const auto workspaceResult = applyDeviceQuantityCached(store, changedWorkspace, 2, cache, order);
    assert(workspaceResult.ok);
    assert(workspaceResult.appliedDelta == -1);
    assert(store.items().front().quantity == 0);
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

    // One decoder serves event fields and result acknowledgements: escapes become UTF-8, and the same
    // identifier decodes to the same string in both places.
    const auto syncWith = [](const string& eventId, const string& code, const string& ack) {
      return R"({"protocolVersion":1,"requestId":"sync-json","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":1,"events":[{"eventId":")" +
             eventId + R"(","type":"inventory.adjust","code":")" + code +
             R"(","value":1}],"resultAcks":[")" + ack + R"("]})";
    };
    assert(parseDeviceSyncRequestJson(syncWith(R"(a\nb\tc\b\f\"\\\/)", R"(µF)", R"(a\nb\tc\b\f\"\\\/)"),
                                      request, error));
    assert(request.events.front().eventId == "a\nb\tc\b\f\"\\/");
    assert(request.resultAcks.front() == request.events.front().eventId);
    assert(request.events.front().code == "\xC2\xB5" "F");
    assert(parseDeviceSyncRequestJson(syncWith("e1", R"(€😀)", "e1-result"), request, error));
    assert(request.events.front().code == "\xE2\x82\xAC\xF0\x9F\x98\x80");
    assert(parseDeviceSyncRequestJson(syncWith("e1", "\xC2\xB5" "F", "e1-result"), request, error));
    assert(request.events.front().code == "\xC2\xB5" "F");
    assert(parseDeviceSyncRequestJson(syncWith(R"(idé)", "0002", R"(idé)"), request, error));
    assert(request.events.front().eventId == "id\xC3\xA9" && request.resultAcks.front() == "id\xC3\xA9");
    // A NUL escape, unpaired surrogates and raw control characters are refused, not altered.
    assert(!parseDeviceSyncRequestJson(syncWith("e1", R"(ab\u0000c)", "e1-result"), request, error));
    assert(!parseDeviceSyncRequestJson(syncWith("e1", "0002", R"(ack\u0000)"), request, error));
    assert(!parseDeviceSyncRequestJson(syncWith("e1", R"(\ud83d)", "e1-result"), request, error));
    assert(!parseDeviceSyncRequestJson(syncWith("e1", R"(\ud83dx)", "e1-result"), request, error));
    assert(!parseDeviceSyncRequestJson(syncWith("e1", R"(\ude00)", "e1-result"), request, error));
    assert(!parseDeviceSyncRequestJson(syncWith("e1", "0002", string("a") + '\x01' + "b"), request, error));
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

    DeviceSyncResponse realisticResponse;
    realisticResponse.requestId = "Inventatory-SCAN-R1-sync-42";
    realisticResponse.acceptedEventIds = {"Inventatory-SCAN-R1-41"};
    realisticResponse.results.push_back({"Inventatory-SCAN-R1-41-result", "Inventatory-SCAN-R1-41", "",
                                         "completed", false, "10k resistor", -3, -3, 17, "R2-B4", "0002",
                                         "Quantity updated"});
    realisticResponse.hasLookupResult = true;
    realisticResponse.lookupResult = {"lookup-9", "found", "10k resistor"};
    realisticResponse.hasQuickLabels = true;
    realisticResponse.quickLabelRevision = 7;
    realisticResponse.quickLabelPresets = {"5V", "12V", "GND"};
    const auto realisticResponseJson = deviceSyncResponseJson(realisticResponse);
    assert(realisticResponseJson.size() <= kInventatoryScanResponseBodyLimit);

    DeviceSyncResponse maximumResponse;
    maximumResponse.requestId = string(96, 'R');
    maximumResponse.acceptedEventIds = {string(96, 'A'), string(96, 'B'), string(96, 'C'), string(96, 'D')};
    for (int index = 0; index < 4; ++index) {
      maximumResponse.results.push_back({string(96, static_cast<char>('0' + index)), string(96, 'E'), "",
                                         "completed_with_warning", false, string(4096, 'N'), -1, -1, 1,
                                         string(4096, 'L'), string(128, 'C'), string(4096, 'M')});
    }
    maximumResponse.hasLookupResult = true;
    maximumResponse.lookupResult = {string(96, 'U'), "found", string(4096, 'I')};
    maximumResponse.hasQuickLabels = true;
    maximumResponse.quickLabelRevision = 9;
    maximumResponse.quickLabelPresets.assign(12, string(1024, 'P'));
    maximumResponse.hasQuickLabelPrintResult = true;
    maximumResponse.quickLabelPrintResult = {string(96, 'Q'), "failed", string(1024, 'C'), string(4096, 'M')};
    const auto maximumResponseJson = deviceSyncResponseJson(maximumResponse);
    assert(maximumResponseJson.size() <= kInventatoryScanResponseBodyLimit);
    auto escapedMaximumResponse = maximumResponse;
    const string controlText(4096, '\x01');
    for (auto& result : escapedMaximumResponse.results) {
      result.itemName = controlText;
      result.location = controlText;
      result.message = controlText;
    }
    escapedMaximumResponse.lookupResult.itemName = controlText;
    escapedMaximumResponse.quickLabelPresets.assign(12, controlText);
    escapedMaximumResponse.quickLabelPrintResult.message = controlText;
    const auto escapedMaximumResponseJson = deviceSyncResponseJson(escapedMaximumResponse);
    assert(escapedMaximumResponseJson.size() <= kInventatoryScanResponseBodyLimit);
    DeviceSyncResponse exactBoundaryResponse;
    exactBoundaryResponse.requestId = "R";
    const auto exactBoundaryBase = deviceSyncResponseJson(exactBoundaryResponse);
    exactBoundaryResponse.requestId.append(
        kInventatoryScanResponseBodyLimit - exactBoundaryBase.size(), 'X');
    const auto exactBoundaryJson = deviceSyncResponseJson(exactBoundaryResponse);
    assert(exactBoundaryJson.size() == kInventatoryScanResponseBodyLimit);
    exactBoundaryResponse.requestId.push_back('X');
    assert(deviceSyncResponseJson(exactBoundaryResponse).size() == kInventatoryScanResponseBodyLimit + 1U);
    cout << "Scan R1 response sizes: realistic=" << realisticResponseJson.size()
         << " maximum=" << maximumResponseJson.size() << " escaped-maximum="
         << escapedMaximumResponseJson.size() << " exact=" << exactBoundaryJson.size() << " limit="
         << kInventatoryScanResponseBodyLimit << '\n';

    // The baseline accepts only protocol v1 envelopes.
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":99,"requestId":"sync-2","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[]})",
        request, error));
    assert(error == "Unsupported protocol version");
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-malformed-array","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[1],"resultAcks":[]})",
        request, error));
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-malformed-ack","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[1]})",
        request, error));
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-duplicate-event","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":2,"events":[{"eventId":"same","type":"inventory.adjust","code":"0002","value":1},{"eventId":"same","type":"inventory.adjust","code":"0002","value":1}],"resultAcks":[]})",
        request, error));
    assert(!parseDeviceSyncRequestJson(
        R"({"protocolVersion":1,"requestId":"sync-missing-comma","deviceId":"r1-a","firmwareVersion":"0.1.0" "mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[]})",
        request, error));
    string deeplyNested = R"({"protocolVersion":1,"requestId":"sync-deep","deviceId":"r1-a","firmwareVersion":"0.1.0","mode":"ready","rssi":-48,"queueDepth":0,"events":[],"resultAcks":[],"extra":)";
    deeplyNested.append(33, '[');
    deeplyNested += "0";
    deeplyNested.append(33, ']');
    deeplyNested += '}';
    assert(!parseDeviceSyncRequestJson(deeplyNested, request, error));

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
    assert(pending.front().deviceId == "r1-a");

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
    const vector<PrinterQueueInfo> queues = {
        {"Queue A", "Driver A", "PORTA", "Ready", false, true},
        {"Queue B", "Driver B", "PORTB", "Ready", false, true},
    };
    string draft = "Existing queue";
    bool dirty = false;
    assert(stagePrinterQueueSelection(queues, 1, draft, dirty));
    assert(draft == "Queue B");
    assert(dirty);

    draft = "Existing queue";
    dirty = false;
    assert(!stagePrinterQueueSelection(queues, queues.size(), draft, dirty));
    assert(draft == "Existing queue");
    assert(!dirty);
  }

  {
    int parsed = 0;
    assert(parseIntegerInRange("42", 0, 100, parsed) && parsed == 42);
    assert(!parseIntegerInRange("42x", 0, 100, parsed));
    assert(!parseIntegerInRange(" 42", 0, 100, parsed));
    assert(!parseIntegerInRange("", 0, 100, parsed));
    assert(!parseIntegerInRange("2147483648", 0, (numeric_limits<int>::max)(), parsed));
    assert(!parseIntegerInRange("-1", 0, 100, parsed));
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
    // Small auxiliary files must validate before publication and preserve the
    // last good bytes when a draft or final replacement is rejected.
    const auto root = filesystem::temp_directory_path() / "inventatory-atomic-auxiliary-test";
    error_code cleanupError;
    filesystem::remove_all(root, cleanupError);
    assert(!cleanupError);
    assert(filesystem::create_directories(root));

    const auto readBytes = [](const filesystem::path& path) {
      ifstream input(path, ios::binary);
      return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
    };

    AppSettings original;
    original.dataDirectory = root / "unicode-данные-测试";
    original.printerQueue = "Queue";
    const auto settingsPath = root / "settings.conf";
    assert(saveAppSettings(settingsPath, original));
    const auto settingsBytes = readBytes(settingsPath);

    auto oversizedSettings = original;
    oversizedSettings.printerQueue.assign(1025, 'x');
    assert(!saveAppSettings(settingsPath, oversizedSettings));
    assert(readBytes(settingsPath) == settingsBytes);

    const auto replacementTarget = root / "replacement-target";
    assert(filesystem::create_directory(replacementTarget));
    assert(!saveAppSettings(replacementTarget, original));
    assert(filesystem::is_directory(replacementTarget));

    const auto quickPath = quickLabelsPath(original.dataDirectory);
    const vector<string> labels = {"5V", "GND"};
    assert(saveQuickLabels(quickPath, labels, 4));
    const auto quickBytes = readBytes(quickPath);
    auto tooManyLabels = labels;
    tooManyLabels.resize(kQuickLabelPresetLimit + 1, "extra");
    assert(!saveQuickLabels(quickPath, tooManyLabels, 4));
    assert(readBytes(quickPath) == quickBytes);
    assert(!saveQuickLabels(quickPath, {string(kQuickLabelPresetTextLimit + 1, 'x')}, 4));
    assert(readBytes(quickPath) == quickBytes);

    const auto malformedQuickPath = root / "malformed-quick-labels.conf";
    {
      ofstream malformed(malformedQuickPath, ios::binary | ios::trunc);
      malformed << "quick_label=\"unterminated\n";
    }
    vector<string> preservedLabels = labels;
    uint32_t preservedRevision = 4;
    assert(!loadQuickLabels(malformedQuickPath, preservedLabels, preservedRevision));
    assert(preservedLabels == labels);
    assert(preservedRevision == 4);
    {
      ofstream oversized(malformedQuickPath, ios::binary | ios::trunc);
      oversized << string(16U * 1024U + 1U, 'x');
    }
    assert(!loadQuickLabels(malformedQuickPath, preservedLabels, preservedRevision));

    const auto activityPath = root / "activity.tsv";
    const vector<ActivityEntry> activities = {{123, "scan", "unicode-данные-测试"}};
    assert(saveActivities(activityPath, activities));
    const auto activityBytes = readBytes(activityPath);
    assert(!saveActivities(activityPath, {{123, "bad\nkind", "message"}}));
    assert(readBytes(activityPath) == activityBytes);
    const auto activityReplacementTarget = root / "activity-replacement-target";
    assert(filesystem::create_directory(activityReplacementTarget));
    assert(!saveActivities(activityReplacementTarget, activities));
    assert(filesystem::is_directory(activityReplacementTarget));

    AppSettings loadedSettings;
    assert(loadAppSettings(settingsPath, loadedSettings));
    assert(loadedSettings.dataDirectory == original.dataDirectory);
    vector<string> loadedLabels;
    uint32_t loadedRevision = 1;
    assert(loadQuickLabels(quickPath, loadedLabels, loadedRevision));
    assert(loadedLabels == labels);
    assert(loadedRevision == 4);
    vector<ActivityEntry> loadedActivities;
    assert(loadActivities(activityPath, loadedActivities));
    assert(loadedActivities.size() == activities.size());
    assert(loadedActivities[0].timestamp == activities[0].timestamp);
    assert(loadedActivities[0].kind == activities[0].kind);
    {
      ofstream trailingActivity(activityPath, ios::binary | ios::trunc);
      trailingActivity << "123 \"scan\" \"valid prefix\" trailing-garbage\n";
    }
    loadedActivities.clear();
    assert(!loadActivities(activityPath, loadedActivities));
    assert(saveActivities(activityPath, activities));
    assert(loadActivities(activityPath, loadedActivities));
    assert(loadedActivities[0].message == activities[0].message);
    {
      ofstream malformed(activityPath, ios::binary | ios::trunc);
      malformed << "123 \"scan\" \"unterminated\n";
    }
    const vector<ActivityEntry> preservedActivities = loadedActivities;
    assert(!loadActivities(activityPath, loadedActivities));
    assert(loadedActivities.size() == preservedActivities.size());
    assert(loadedActivities[0].message == preservedActivities[0].message);

    filesystem::remove_all(root, cleanupError);
    assert(!cleanupError);
  }

  {
    const auto executablePath = L"C:\\Program Files\\Inventatory\\inventatory.exe";
    const auto launcherPath = buildBackgroundStartupLauncherPath(executablePath);
    assert(launcherPath == L"C:\\Program Files\\Inventatory\\inventatory-background.exe");
    assert(buildBackgroundStartupCommand(launcherPath) ==
           L"\"C:\\Program Files\\Inventatory\\inventatory-background.exe\" --background");
    assert(buildDesktopShortcutPath(L"C:\\Users\\pawci\\Desktop") ==
           L"C:\\Users\\pawci\\Desktop\\Inventatory.lnk");
    assert(buildDesktopShortcutPath(L"C:\\Users\\pawci\\Desktop\\") ==
           L"C:\\Users\\pawci\\Desktop\\Inventatory.lnk");
  }

#ifndef _WIN32
  {
    const auto tempDir = filesystem::temp_directory_path() / ("inventatory-shortcut-test-" + to_string(getpid()));
    const auto testData = tempDir / "data";
    const auto testConfig = tempDir / "config";
    const auto testDesktop = tempDir / "desktop";
    filesystem::create_directories(testData);
    filesystem::create_directories(testConfig);
    filesystem::create_directories(testDesktop);

    const auto oldData = getenv("XDG_DATA_HOME");
    const auto oldConfig = getenv("XDG_CONFIG_HOME");
    const auto oldDesktop = getenv("XDG_DESKTOP_DIR");

    setenv("XDG_DATA_HOME", testData.c_str(), 1);
    setenv("XDG_CONFIG_HOME", testConfig.c_str(), 1);
    setenv("XDG_DESKTOP_DIR", testDesktop.c_str(), 1);

    string shortcutError;
    assert(createDesktopShortcut(shortcutError));
    assert(shortcutError.empty());

    const auto launcher = testData / "applications" / "inventatory.desktop";
    assert(filesystem::is_regular_file(launcher));
    ifstream launcherStream(launcher);
    const string launcherContent((istreambuf_iterator<char>(launcherStream)), istreambuf_iterator<char>());
    assert(launcherContent.find("Icon=inventatory") != string::npos);
    assert(launcherContent.find("Terminal=false") != string::npos);
    assert(launcherContent.find("StartupWMClass=inventatory") != string::npos);
    assert(launcherContent.find("[Desktop Entry]") != string::npos);

    // A second launch must not replace the launcher or icons; a replaced launcher makes Plasma
    // revert the taskbar icon of the already running window.
    const auto iconPath = testData / "icons" / "hicolor" / "48x48" / "apps" / "inventatory.png";
    struct stat launcherBefore{}, iconBefore{}, desktopBefore{};
    const auto desktopLauncherPath = testDesktop / "inventatory.desktop";
    assert(stat(launcher.c_str(), &launcherBefore) == 0);
    assert(stat(iconPath.c_str(), &iconBefore) == 0);
    assert(stat(desktopLauncherPath.c_str(), &desktopBefore) == 0);
    assert(createDesktopShortcut(shortcutError));
    struct stat launcherAfter{}, iconAfter{}, desktopAfter{};
    assert(stat(launcher.c_str(), &launcherAfter) == 0);
    assert(stat(iconPath.c_str(), &iconAfter) == 0);
    assert(stat(desktopLauncherPath.c_str(), &desktopAfter) == 0);
    assert(launcherAfter.st_ino == launcherBefore.st_ino);
    assert(iconAfter.st_ino == iconBefore.st_ino);
    assert(desktopAfter.st_ino == desktopBefore.st_ino);
    {
      ofstream stale(launcher, ios::trunc);
      stale << "[Desktop Entry]\nName=stale\n";
    }
    assert(createDesktopShortcut(shortcutError));
    ifstream repaired(launcher);
    const string repairedContent((istreambuf_iterator<char>(repaired)), istreambuf_iterator<char>());
    assert(repairedContent.find("Icon=inventatory") != string::npos);

    char* testArgv[] = {const_cast<char*>("inventatory"), nullptr};
    if (isatty(STDIN_FILENO) != 0) {
      assert(ensureTerminalAttached(1, testArgv));
    }

    const auto konsoleArgs = buildTerminalCandidateArgs("konsole", "/usr/bin/inventatory", {"--debug"});
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--separate") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--hide-menubar") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--hide-tabbar") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--hide-toolbars") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--desktopfile") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "-qwindowicon") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "Icon=inventatory") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "LocalTabTitleFormat=%w") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "ScrollBarPosition=2") != konsoleArgs.end());
    assert(find(konsoleArgs.begin(), konsoleArgs.end(), "--debug") != konsoleArgs.end());

    const auto gnomeArgs = buildTerminalCandidateArgs("gnome-terminal", "/usr/bin/inventatory");
    assert(find(gnomeArgs.begin(), gnomeArgs.end(), "--hide-menubar") != gnomeArgs.end());
    assert(find(gnomeArgs.begin(), gnomeArgs.end(), "--class=inventatory") != gnomeArgs.end());

    const auto ptyxisArgs = buildTerminalCandidateArgs("ptyxis", "/usr/bin/inventatory");
    assert(find(ptyxisArgs.begin(), ptyxisArgs.end(), "--standalone") != ptyxisArgs.end());
    assert(find(ptyxisArgs.begin(), ptyxisArgs.end(), "--app-id=inventatory") != ptyxisArgs.end());

    const auto xtermArgs = buildTerminalCandidateArgs("xterm", "/usr/bin/inventatory");
    assert(find(xtermArgs.begin(), xtermArgs.end(), "+sb") != xtermArgs.end());
    assert(find(xtermArgs.begin(), xtermArgs.end(), "-class") != xtermArgs.end());

    const auto desktopLauncher = testDesktop / "inventatory.desktop";
    assert(filesystem::is_regular_file(desktopLauncher));

    const vector<int> expectedSizes = {16, 24, 32, 48, 64, 128, 256, 512};
    for (int size : expectedSizes) {
      const auto iconPath = testData / "icons" / "hicolor" / (to_string(size) + "x" + to_string(size)) / "apps" / "inventatory.png";
      assert(filesystem::is_regular_file(iconPath));
      ifstream iconStream(iconPath, ios::binary);
      char header[8] = {};
      iconStream.read(header, 8);
      assert(memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0);
    }

    if (oldData) setenv("XDG_DATA_HOME", oldData, 1); else unsetenv("XDG_DATA_HOME");
    if (oldConfig) setenv("XDG_CONFIG_HOME", oldConfig, 1); else unsetenv("XDG_CONFIG_HOME");
    if (oldDesktop) setenv("XDG_DESKTOP_DIR", oldDesktop, 1); else unsetenv("XDG_DESKTOP_DIR");

    error_code cleanupEc;
    filesystem::remove_all(tempDir, cleanupEc);
  }
#endif

  {
    const auto icoPath = filesystem::path("branding") / "icons" / "inventatory.ico";
    if (filesystem::is_regular_file(icoPath)) {
      ifstream icoStream(icoPath, ios::binary);
      uint16_t reserved = 0, type = 0, count = 0;
      icoStream.read(reinterpret_cast<char*>(&reserved), 2);
      icoStream.read(reinterpret_cast<char*>(&type), 2);
      icoStream.read(reinterpret_cast<char*>(&count), 2);
      assert(reserved == 0 && type == 1 && count == 7);
      for (int i = 0; i < count; ++i) {
        icoStream.seekg(6 + i * 16);
        uint8_t w = 0, h = 0, colors = 0, res = 0;
        uint16_t planes = 0, bpp = 0;
        uint32_t bytesInRes = 0, imageOffset = 0;
        icoStream.read(reinterpret_cast<char*>(&w), 1);
        icoStream.read(reinterpret_cast<char*>(&h), 1);
        icoStream.read(reinterpret_cast<char*>(&colors), 1);
        icoStream.read(reinterpret_cast<char*>(&res), 1);
        icoStream.read(reinterpret_cast<char*>(&planes), 2);
        icoStream.read(reinterpret_cast<char*>(&bpp), 2);
        icoStream.read(reinterpret_cast<char*>(&bytesInRes), 4);
        icoStream.read(reinterpret_cast<char*>(&imageOffset), 4);
        assert(bpp == 32);
        icoStream.seekg(imageOffset);
        if (w == 0 && h == 0) {
          char magic[4] = {};
          icoStream.read(magic, 4);
          assert(memcmp(magic, "\x89PNG", 4) == 0);
        } else {
          uint32_t biSize = 0;
          int32_t biWidth = 0, biHeight = 0;
          uint16_t biPlanes = 0, biBitCount = 0;
          uint32_t biCompression = 0;
          icoStream.read(reinterpret_cast<char*>(&biSize), 4);
          icoStream.read(reinterpret_cast<char*>(&biWidth), 4);
          icoStream.read(reinterpret_cast<char*>(&biHeight), 4);
          icoStream.read(reinterpret_cast<char*>(&biPlanes), 2);
          icoStream.read(reinterpret_cast<char*>(&biBitCount), 2);
          icoStream.read(reinterpret_cast<char*>(&biCompression), 4);
          assert(biSize == 40);
          assert(biWidth == w);
          assert(biHeight == h * 2);
          assert(biPlanes == 1);
          assert(biBitCount == 32);
          assert(biCompression == 0);
        }
      }
    }
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
    assert(!loadAppSettings(path, loaded));
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    // The EU default is not written, so the file stays readable by releases that predate the setting.
    const auto path = filesystem::temp_directory_path() / "inventatory-symbol-standard-settings-test.conf";
    AppSettings defaults;
    defaults.dataDirectory = "data";
    assert(saveAppSettings(path, defaults));
    {
      ifstream saved(path);
      const string text((istreambuf_iterator<char>(saved)), istreambuf_iterator<char>());
      assert(text.find("symbol_standard") == string::npos);
    }
    AppSettings loaded;
    loaded.symbolStandard = SymbolStandard::Us;
    assert(loadAppSettings(path, loaded) && loaded.symbolStandard == SymbolStandard::Eu);

    defaults.symbolStandard = SymbolStandard::Us;
    assert(saveAppSettings(path, defaults));
    string savedText;
    {
      ifstream saved(path);
      savedText.assign((istreambuf_iterator<char>(saved)), istreambuf_iterator<char>());
    }
    assert(savedText.find("symbol_standard=\"us\"\n") != string::npos);
    AppSettings reloaded;
    assert(loadAppSettings(path, reloaded) && reloaded.symbolStandard == SymbolStandard::Us);

    // An unknown value and a repeated key both invalidate the file rather than silently falling back.
    const auto writeWith = [&](const string& extra) {
      ofstream out(path, ios::trunc);
      out << savedText.substr(0, savedText.find("symbol_standard=")) << extra;
    };
    writeWith("symbol_standard=\"iso\"\n");
    assert(!loadAppSettings(path, reloaded));
    writeWith("symbol_standard=\"us\"\nsymbol_standard=\"eu\"\n");
    assert(!loadAppSettings(path, reloaded));
    writeWith("symbol_standard=us\n");
    assert(!loadAppSettings(path, reloaded));
    error_code removeError;
    filesystem::remove(path, removeError);
    assert(!removeError);
  }

  {
    const auto previewHints = updatePreviewControlHints();
    assert(string(previewHints.title) == "Inventatory Update Available");
    assert(updatePreviewVersionLine("0.1.0", "v0.2.0-rc.2") == "0.1.0 -> v0.2.0-rc.2");
    assert(string(previewHints.releaseNotesHeading) == "Release notes");
    assert(string(previewHints.start) == "[Enter]");
    assert(string(previewHints.cancel) == "[Esc]");
    assert(string(previewHints.readNotes) == "[↑↓] Read notes");
    assert(updatePreviewStartsOnKey(KeyEvent{KeyType::Enter, '\0'}));
    assert(!updatePreviewStartsOnKey(KeyEvent{KeyType::Character, 's'}));
    assert(!updatePreviewStartsOnKey(KeyEvent{KeyType::Character, 'S'}));

    assert(cleanMarkdownInline("[PR #12](https://github.com/pull/12)") == "PR #12");
    assert(cleanMarkdownInline("**Notice:** See [`readme`](https://example.com)") == "Notice: See readme");

    const auto emptyNotes = formatUpdateNotes("", 50);
    assert(emptyNotes.size() == 1);
    assert(emptyNotes[0].kind == UpdateNoteLineKind::Paragraph);
    assert(emptyNotes[0].text == "No release notes were provided for this release.");

    const string sampleMarkdown =
        "### Highlights\n"
        "- Added brand new application icon.\n"
        "- Refined rack labels to make barcode scanning much easier on long shelves.\n"
        "\n"
        "### Fixes\n"
        "- Resolved [`locale bug`](https://github.com/issues/1) on Linux.";

    const auto formatted = formatUpdateNotes(sampleMarkdown, 45);
    assert(!formatted.empty());
    assert(formatted[0].kind == UpdateNoteLineKind::Heading);
    assert(formatted[0].text == "Highlights");
    assert(formatted[1].kind == UpdateNoteLineKind::BulletStart);
    assert(formatted[1].text == "Added brand new application icon.");
    // Long bullet should have wrapped into BulletStart followed by BulletContinuation
    assert(formatted[2].kind == UpdateNoteLineKind::BulletStart);
    assert(formatted[3].kind == UpdateNoteLineKind::BulletContinuation);
    // Followed by empty spacer line
    size_t fixesHeadingIndex = 0;
    for (size_t i = 0; i < formatted.size(); ++i) {
      if (formatted[i].kind == UpdateNoteLineKind::Heading && formatted[i].text == "Fixes") {
        fixesHeadingIndex = i;
        break;
      }
    }
    assert(fixesHeadingIndex > 0);
    assert(formatted[fixesHeadingIndex - 1].kind == UpdateNoteLineKind::Empty);
    assert(formatted[fixesHeadingIndex + 1].kind == UpdateNoteLineKind::BulletStart);
    assert(formatted[fixesHeadingIndex + 1].text == "Resolved locale bug on Linux.");
  }

  {
    // Typed characters keep their full UTF-8 sequence and Backspace removes
    // whole characters, so Polish letters and unit symbols never corrupt text.
    string buffer;
    appendKeyText(buffer, KeyEvent{KeyType::Character, 'a'});
    appendKeyText(buffer, KeyEvent{KeyType::Character, '\0', "\xC5\x82"});      // ł
    appendKeyText(buffer, KeyEvent{KeyType::Character, '\0', "\xC2\xB5"});      // µ
    appendKeyText(buffer, KeyEvent{KeyType::Character, '\0', "\xE2\x84\xA6"});  // Ω
    assert(buffer == "a\xC5\x82\xC2\xB5\xE2\x84\xA6");
    eraseLastCharacter(buffer);
    assert(buffer == "a\xC5\x82\xC2\xB5");
    eraseLastCharacter(buffer);
    eraseLastCharacter(buffer);
    assert(buffer == "a");
    eraseLastCharacter(buffer);
    assert(buffer.empty());
    eraseLastCharacter(buffer);
    assert(buffer.empty());
    appendKeyText(buffer, KeyEvent{KeyType::Character, '\0'});
    assert(buffer.empty());
  }

  {
    // CUPS lpstat -p lines (parsed under LC_ALL=C): a disabled queue must not be ready.
    const auto idle = parseCupsQueueLine("printer Zebra is idle.  enabled since Tue 05 Oct 2026 10:00:00");
    assert(idle.has_value());
    assert(idle->name == "Zebra");
    assert(idle->ready);
    assert(idle->status.rfind("idle.", 0) == 0);

    const auto printing = parseCupsQueueLine("printer Zebra now printing Zebra-12.  enabled since Tue 05 Oct 2026 10:00:00");
    assert(printing.has_value());
    assert(printing->name == "Zebra");
    assert(printing->ready);

    const auto disabled = parseCupsQueueLine("printer Zebra disabled since Tue 05 Oct 2026 10:00:00 -");
    assert(disabled.has_value());
    assert(disabled->name == "Zebra");
    assert(!disabled->ready);

    assert(!parseCupsQueueLine("system default destination: Zebra").has_value());
    assert(!parseCupsQueueLine("\treason unknown").has_value());
    assert(!parseCupsQueueLine("").has_value());
    assert(!parseCupsQueueLine("printer ").has_value());
    assert(!parseCupsQueueLine("printer Zebra").has_value());
  }

  {
    // A parameter name without ASCII letters/digits must not match every lookup.
    assert(!parameterLabelMatches("\xE7\x94\xB5\xE5\xAE\xB9", "Package"));  // CJK name
    assert(!parameterLabelMatches("Package", "\xE7\x94\xB5\xE5\xAE\xB9"));
    assert(!parameterLabelMatches("", "Package"));
    assert(parameterLabelMatches("Package / Case", "Package"));
    assert(parameterLabelMatches("Capacitance", "capacitance"));
  }

  {
    assert(isVersionNewer("v0.1.1", "0.1.0"));
    assert(isVersionNewer("0.1.1", "0.1.0"));
    assert(isVersionNewer("v1.2.3.1", "1.2.3"));
    assert(!isVersionNewer("0.1.0", "0.1.0"));
    assert(!isVersionNewer("preview", "0.1.0"));
    assert(isUpdateCheckDue(true, 0, 100));
    assert(!isUpdateCheckDue(false, 0, 100));
    assert(!isUpdateCheckDue(true, 100, 100 + 60));
    assert(isUpdateCheckDue(true, 100, 100 + 24 * 60 * 60));
    assert(updateEtaSeconds(50, 100, 10.0) == 5.0);
    assert(updateEtaSeconds(100, 100, 10.0) == 0.0);
    assert(updateEtaSeconds(0, 0, 10.0) == 0.0);
  }

  {
    const string releaseJson = currentPlatformReleaseFixture(
        R"({
          "tag_name": "v1.2.3",
          "html_url": "https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.3",
          "body": "Fixes\n\u2605 safer updates",
          "assets": [
            {"name": "Inventatory-win-x64.zip"},
            {"name": "SHA256SUMS.txt"},
            {"name": "Install-Inventatory.ps1"}
          ]
        })");
    const auto metadata = parseReleaseMetadata(releaseJson, "1.2.0", "Kwiatens/Inventatory-Software");
    assert(metadata.completed);
    assert(metadata.updateAvailable);
    assert(metadata.latestVersion == "v1.2.3");
    assert(metadata.releaseNotes == "Fixes\n\xE2\x98\x85 safer updates");

    const auto prereleaseMetadata = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            R"([{"tag_name":"v1.2.4-rc.1","html_url":"https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.4-rc.1","body":"candidate","assets":[{"name":"Inventatory-win-x64.zip"},{"name":"SHA256SUMS.txt"},{"name":"Install-Inventatory.ps1"}]}])"),
        "1.2.3", "Kwiatens/Inventatory-Software");
    assert(prereleaseMetadata.completed);
    assert(prereleaseMetadata.updateAvailable);
    assert(prereleaseMetadata.latestVersion == "v1.2.4-rc.1");
    const auto nestedUrlMetadata = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            R"({"tag_name":"v1.2.5","html_url":"https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.5","body":"notes","author":{"html_url":"https://github.com/example"},"assets":[{"name":"Inventatory-win-x64.zip","uploader":{"html_url":"https://github.com/example"}},{"name":"SHA256SUMS.txt"},{"name":"Install-Inventatory.ps1"}]})"),
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(nestedUrlMetadata.completed);
    assert(nestedUrlMetadata.updateAvailable);
    assert(nestedUrlMetadata.latestVersion == "v1.2.5");
    const auto emptyOptionalChannel = parseReleaseMetadata(
        "[]", "0.1.0", "Kwiatens/Inventatory-Hardware", false);
    assert(emptyOptionalChannel.completed);
    assert(!emptyOptionalChannel.updateAvailable);
    assert(emptyOptionalChannel.latestVersion.empty());
    assert(isVersionNewer("v1.2.0-rc.2", "v1.2.0-rc.1"));
    assert(isVersionNewer("v1.2.0", "v1.2.0-rc.1"));
    assert(!isVersionNewer("v1.2.0-rc.1", "v1.2.0"));

    const auto missingAsset = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            R"({"tag_name":"v1.2.3","html_url":"https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.3","body":"notes","assets":[{"name":"Inventatory-win-x64.zip"},{"name":"SHA256SUMS.txt"}]})"),
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(!missingAsset.completed);
    const auto invalidTag = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            R"({"tag_name":"release","html_url":"https://github.com/Kwiatens/Inventatory-Software/releases/tag/release","body":"notes","assets":[{"name":"Inventatory-win-x64.zip"},{"name":"SHA256SUMS.txt"},{"name":"Install-Inventatory.ps1"}]})"),
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(!invalidTag.completed);
    const auto invalidUrl = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            R"({"tag_name":"v1.2.3","html_url":"https://example.com/release","body":"notes","assets":[{"name":"Inventatory-win-x64.zip"},{"name":"SHA256SUMS.txt"},{"name":"Install-Inventatory.ps1"}]})"),
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(!invalidUrl.completed);
    string oversizedNotes(64U * 1024U + 1U, 'x');
    const auto oversized = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            "{\"tag_name\":\"v1.2.3\",\"html_url\":\"https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.3\",\"body\":\"" +
            oversizedNotes +
            "\",\"assets\":[{\"name\":\"Inventatory-win-x64.zip\"},{\"name\":\"SHA256SUMS.txt\"},{\"name\":\"Install-Inventatory.ps1\"}]}") ,
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(!oversized.completed);
    const auto malformedNotes = parseReleaseMetadata(
        currentPlatformReleaseFixture(
            "{\"tag_name\":\"v1.2.3\",\"html_url\":\"https://github.com/Kwiatens/Inventatory-Software/releases/tag/v1.2.3\",\"body\":\"bad\nnotes\",\"assets\":[{\"name\":\"Inventatory-win-x64.zip\"},{\"name\":\"SHA256SUMS.txt\"},{\"name\":\"Install-Inventatory.ps1\"}]}") ,
        "1.2.0", "Kwiatens/Inventatory-Software");
    assert(!malformedNotes.completed);

    assert(buildReleaseAssetUrl("Kwiatens/Inventatory-Software", "v1.2.3", kTestApplicationArchive) ==
           string("https://github.com/Kwiatens/Inventatory-Software/releases/download/v1.2.3/") + kTestApplicationArchive);
    assert(buildReleaseAssetUrl("Kwiatens/Inventatory-Software", "v1.2.3-rc.1", kTestApplicationArchive) ==
           string("https://github.com/Kwiatens/Inventatory-Software/releases/download/v1.2.3-rc.1/") + kTestApplicationArchive);
    assert(buildReleaseAssetUrl("Kwiatens/Inventatory-Software", "v0.2.0-rc.2", kTestApplicationArchive) ==
           string("https://github.com/Kwiatens/Inventatory-Software/releases/download/v0.2.0-rc.2/") + kTestApplicationArchive);
    assert(buildReleaseAssetUrl("Kwiatens/Inventatory-Software", "v1.2", kTestApplicationArchive).empty());
    assert(buildReleaseAssetUrl("evil/repo/extra", "v1.2.3", kTestApplicationArchive).empty());

    string downloadError;
    assert(!downloadReleaseAsset(string("https://example.invalid/") + kTestApplicationArchive,
                                 filesystem::temp_directory_path() / "inventatory-update-url-test.zip", {}, downloadError));
    assert(downloadError == "The update asset URL is not an approved GitHub download");

    const string archiveHash(64U, 'a');
    const string installerHash(64U, 'b');
    string expectedChecksum = string(archiveHash + "  ") + kTestApplicationArchive + "\n" + installerHash + " *" +
                              kTestApplicationInstaller + "\n";
    string parsedHash;
    assert(parseSha256Checksum(expectedChecksum, kTestApplicationArchive, parsedHash));
    assert(parsedHash == archiveHash);
    assert(parseSha256Checksum(expectedChecksum, kTestApplicationInstaller, parsedHash));
    assert(parsedHash == installerHash);
    assert(!parseSha256Checksum(expectedChecksum, kTestApplicationChecksums, parsedHash));
    assert(!parseSha256Checksum(string(archiveHash + "  ") + kTestApplicationArchive + "\n" + archiveHash + "  " +
                                kTestApplicationArchive + "\n", kTestApplicationArchive, parsedHash));

    const auto hashPath = filesystem::temp_directory_path() / "inventatory-update-hash-test.txt";
    ofstream hashFile(hashPath, ios::binary | ios::trunc);
    hashFile << "abc";
    hashFile.close();
    string hashError;
    assert(verifyReleaseFileSha256(hashPath,
                                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hashError));
    assert(!verifyReleaseFileSha256(hashPath, string(64U, '0'), hashError));
    error_code removeError;
    filesystem::remove(hashPath, removeError);
    assert(!removeError);
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

    {
      SqliteConnection connection;
      assert(openDatabase(path, connection));
      assert(execSql(connection, "UPDATE inventatory_bom_projects SET overrides='v1:malformed'"));
    }
    loaded.clear();
    assert(!loadBomProjects(path, loaded));

    // Saving is a full snapshot, so an empty list clears the table.
    assert(saveBomProjects(path, {}));
    vector<BomProject> empty;
    assert(loadBomProjects(path, empty));
    assert(empty.empty());

    filesystem::remove(path, cleanupError);
  }

  {
    const auto source = filesystem::temp_directory_path() / "inventatory-transfer-source";
    const auto bundle = filesystem::temp_directory_path() / "inventatory-transfer-bundle";
    const auto restoreTarget = filesystem::temp_directory_path() / "inventatory-transfer-restore-target";
    const auto invalidOptionalBundle = filesystem::temp_directory_path() / "inventatory-transfer-invalid-optional-bundle";
    const auto invalidBomBundle = filesystem::temp_directory_path() / "inventatory-transfer-invalid-bom-bundle";
    const auto settingsPath = filesystem::temp_directory_path() / "inventatory-transfer-settings.conf";
    const auto targetSettingsPath = filesystem::temp_directory_path() / "inventatory-transfer-target-settings.conf";
    const auto csv = filesystem::temp_directory_path() / "inventatory-transfer-export.csv";
    error_code cleanupError;
    filesystem::remove_all(source, cleanupError);
    filesystem::remove_all(bundle, cleanupError);
    filesystem::remove_all(invalidOptionalBundle, cleanupError);
    filesystem::remove_all(invalidBomBundle, cleanupError);
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
    item.notes = " =HYPERLINK(\"https://example.invalid\")";
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
      ofstream(source / "activity.tsv") << "1710000000 \"test\" \"test activity\"\n";
      ofstream(source / "quick_labels.conf") << "quick_label_revision=1\n";
    }

    string error;
    assert(exportInventoryCsv(store, csv, error));
    ifstream exported(csv);
    const string exportedText((istreambuf_iterator<char>(exported)), istreambuf_iterator<char>());
    assert(exportedText.find("Transfer resistor") != string::npos);
    assert(exportedText.find("Quantity") != string::npos);
    assert(exportedText.find("\"\t =HYPERLINK(\"\"https://example.invalid\"\")\"") != string::npos);
    assert(csvTextCell("=1+1") == "\"\t=1+1\"");
    assert(csvTextCell("+SUM(A1:A2)") == "\"\t+SUM(A1:A2)\"");
    assert(csvTextCell(" -1+1") == "\"\t -1+1\"");
    assert(csvTextCell("@SUM(A1:A2)") == "\"\t@SUM(A1:A2)\"");
    assert(csvTextCell("\n=1+1") == "\"\t\n=1+1\"");
    assert(csvTextCell("\xEF\xBC\x9D" "1+1") == "\"\t\xEF\xBC\x9D" "1+1\"");
    assert(csvTextCell("ordinary text") == "\"ordinary text\"");
    assert(csvTextCell("a,\"b\"") == "\"a,\"\"b\"\"\"");
    AppSettings backupSettings;
    backupSettings.dataDirectory = source;
    backupSettings.completedOnboardingVersion = 1;
    assert(saveAppSettings(settingsPath, backupSettings));
    const bool bundleCreated = createInventatoryBackup(source, settingsPath, bundle, "1.0.0", error);
    if (!bundleCreated) cerr << "Backup creation failed: " << error << '\n';
    assert(bundleCreated);
    assert(filesystem::exists(bundle / "manifest.tsv"));
    assert(filesystem::exists(bundle / "inventory.db"));
    assert(!filesystem::exists(bundle / "inventatory_scan.conf"));
    assert(validateInventatoryBackup(bundle, error));
    {
      SqliteConnection bundleConnection;
      assert(openDatabaseReadOnly(bundle / "inventory.db", bundleConnection));
      SqliteStatement countStatement;
      assert(sqliteApi().prepare_v2(bundleConnection.db,
                                    "SELECT COUNT(*) FROM inventatory_inventory_commits", -1,
                                    &countStatement.stmt, nullptr) == SQLITE_OK);
      assert(sqliteApi().step(countStatement.stmt) == SQLITE_ROW);
      assert(sqliteApi().column_int64(countStatement.stmt, 0) == 2);
    }
    {
      ifstream snapshotInput(bundle / "inventory.db", ios::binary);
      const string snapshotBytes((istreambuf_iterator<char>(snapshotInput)), istreambuf_iterator<char>());
      assert(validateInventatoryBackup(bundle, error));
      ifstream snapshotInputAgain(bundle / "inventory.db", ios::binary);
      const string snapshotBytesAgain((istreambuf_iterator<char>(snapshotInputAgain)), istreambuf_iterator<char>());
      assert(snapshotBytes == snapshotBytesAgain);
    }
    {
      ofstream extra(bundle / "unexpected.txt", ios::binary);
      extra << "must not be silently included";
      extra.close();
      assert(!validateInventatoryBackup(bundle, error));
      filesystem::remove(bundle / "unexpected.txt", cleanupError);
      assert(validateInventatoryBackup(bundle, error));
    }
    {
      ifstream manifestInput(bundle / "manifest.tsv", ios::binary);
      const string manifest((istreambuf_iterator<char>(manifestInput)), istreambuf_iterator<char>());
      ofstream malformed(bundle / "manifest.tsv", ios::binary | ios::trunc);
      malformed << manifest << "unknown\trow\n";
      malformed.close();
      assert(!validateInventatoryBackup(bundle, error));
      ofstream restoredManifest(bundle / "manifest.tsv", ios::binary | ios::trunc);
      restoredManifest << manifest;
      restoredManifest.close();
      assert(validateInventatoryBackup(bundle, error));
    }
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
    {
      ofstream malformedQuickLabels(source / "quick_labels.conf", ios::trunc);
      malformedQuickLabels << "quick_label_revision=1\nunknown=value\n";
      malformedQuickLabels.close();
      assert(!createInventatoryBackup(source, settingsPath, invalidOptionalBundle, "1.0.0", error));
      assert(!filesystem::exists(invalidOptionalBundle));
      ofstream validQuickLabels(source / "quick_labels.conf", ios::trunc);
      validQuickLabels << "quick_label_revision=1\n";
      validQuickLabels.close();
    }
    {
      SqliteConnection malformedBomConnection;
      assert(openDatabase(source / "inventory.db", malformedBomConnection));
      assert(execSql(malformedBomConnection,
                     "INSERT INTO inventatory_bom_projects "
                     "(id,name,source_path,boards,created_at,last_opened,last_built,bom_text,overrides,enrichment) "
                     "VALUES ('malformed-bom','Malformed BOM','',1,1710000000,1710000000,0,'R1','v1:malformed','')"));
    }
    assert(!createInventatoryBackup(source, settingsPath, invalidBomBundle, "1.0.0", error));
    assert(!filesystem::exists(invalidBomBundle));
    {
      SqliteConnection cleanupBomConnection;
      assert(openDatabase(source / "inventory.db", cleanupBomConnection));
      assert(execSql(cleanupBomConnection, "DELETE FROM inventatory_bom_projects WHERE id='malformed-bom'"));
    }
    {
      InventoryTransferTestHooks hooks;
      hooks.renamePath = [](const filesystem::path& sourcePath, const filesystem::path& targetPath, string& injectedError) {
        if (sourcePath.filename().u8string().find(".restore-staging-") != string::npos) {
          injectedError = "injected activation failure";
          return false;
        }
        error_code injectedFilesystemError;
        filesystem::rename(sourcePath, targetPath, injectedFilesystemError);
        if (injectedFilesystemError) {
          injectedError = injectedFilesystemError.message();
          return false;
        }
        return true;
      };
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks));
      InventoryStore stillProtected;
      assert(stillProtected.load(restoreTarget / "inventory.db"));
      assert(stillProtected.items().front().id == "protected-restore-item");
    }
    {
      InventoryTransferTestHooks hooks;
      hooks.saveSettings = [](const filesystem::path&, const AppSettings&, string& injectedError) {
        injectedError = "injected settings activation failure";
        return false;
      };
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks));
      InventoryStore stillProtected;
      assert(stillProtected.load(restoreTarget / "inventory.db"));
      assert(stillProtected.items().front().id == "protected-restore-item");
    }
    {
      InventoryTransferTestHooks hooks;
      bool replacementWorkspaceActive = false;
      hooks.removeAll = [](const filesystem::path& path, string& injectedError) {
        if (path.filename().u8string().find(".restore-old-data-") != string::npos) {
          injectedError = "injected cleanup failure";
          return false;
        }
        error_code injectedFilesystemError;
        filesystem::remove_all(path, injectedFilesystemError);
        if (injectedFilesystemError) {
          injectedError = injectedFilesystemError.message();
          return false;
        }
        return true;
      };
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks,
                                       &replacementWorkspaceActive));
      assert(replacementWorkspaceActive);
      assert(recoverInventatoryRestore(restoreTarget, targetSettingsPath, error));
    }
    {
      // cleanup_pending is the restore commit point. Recovery must tolerate a
      // crash after old data was deleted but before the settings backup.
      InventoryStore cleanupOldStore;
      InventoryItem cleanupOldItem;
      cleanupOldItem.id = "cleanup-old-item";
      cleanupOldItem.partName = "Cleanup old item";
      cleanupOldStore.items().push_back(cleanupOldItem);
      assert(cleanupOldStore.save(restoreTarget / "inventory.db"));
      InventoryTransferTestHooks hooks;
      hooks.removeAll = [](const filesystem::path& path, string& injectedError) {
        if (path.filename().u8string().find(".restore-old-") != string::npos &&
            path.filename().u8string().find(".restore-old-data-") == string::npos) {
          injectedError = "injected interruption after old-data cleanup";
          return false;
        }
        error_code removeError;
        filesystem::remove_all(path, removeError);
        if (removeError) {
          injectedError = removeError.message();
          return false;
        }
        return true;
      };
      bool replacementWorkspaceActive = false;
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks,
                                       &replacementWorkspaceActive));
      assert(replacementWorkspaceActive);
      assert(recoverInventatoryRestore(restoreTarget, targetSettingsPath, error));
      InventoryStore recoveredStore;
      assert(recoveredStore.load(restoreTarget / "inventory.db"));
      assert(recoveredStore.items().front().id == "transfer-item");
    }
    {
      // A crash after every rollback artifact is gone but before journal
      // removal must also leave the committed workspace recoverable.
      InventoryStore cleanupOldStore;
      InventoryItem cleanupOldItem;
      cleanupOldItem.id = "journal-cleanup-old-item";
      cleanupOldItem.partName = "Journal cleanup old item";
      cleanupOldStore.items().push_back(cleanupOldItem);
      assert(cleanupOldStore.save(restoreTarget / "inventory.db"));
      const auto targetJournal = targetSettingsPath.parent_path() /
                                 filesystem::u8path(targetSettingsPath.filename().u8string() + ".restore-journal");
      InventoryTransferTestHooks hooks;
      hooks.removeAll = [&targetJournal](const filesystem::path& path, string& injectedError) {
        if (path == targetJournal) {
          injectedError = "injected journal cleanup interruption";
          return false;
        }
        error_code removeError;
        filesystem::remove_all(path, removeError);
        if (removeError) {
          injectedError = removeError.message();
          return false;
        }
        return true;
      };
      bool replacementWorkspaceActive = false;
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks,
                                       &replacementWorkspaceActive));
      assert(replacementWorkspaceActive);
      assert(filesystem::exists(targetJournal));
      assert(recoverInventatoryRestore(restoreTarget, targetSettingsPath, error));
      assert(!filesystem::exists(targetJournal));
      InventoryStore recoveredStore;
      assert(recoveredStore.load(restoreTarget / "inventory.db"));
      assert(recoveredStore.items().front().id == "transfer-item");
    }
    {
      // A failure after the settings backup has been created must clean that
      // owned artifact along with staging, without touching the active data.
      InventoryTransferTestHooks hooks;
      bool settingsBackupCreated = false;
      hooks.copyFile = [&targetSettingsPath, &settingsBackupCreated](const filesystem::path& sourcePath,
                                                                       const filesystem::path& targetPath,
                                                                       string& injectedError) {
        error_code injectedFilesystemError;
        filesystem::copy_file(sourcePath, targetPath, filesystem::copy_options::overwrite_existing,
                              injectedFilesystemError);
        if (injectedFilesystemError) {
          injectedError = injectedFilesystemError.message();
          return false;
        }
        if (sourcePath == targetSettingsPath) {
          settingsBackupCreated = true;
          injectedError = "injected settings-backup staging failure";
          return false;
        }
        return true;
      };
      assert(!restoreInventatoryBackup(bundle, restoreTarget, targetSettingsPath, error, &hooks));
      assert(settingsBackupCreated);
      const auto targetJournal = targetSettingsPath.parent_path() /
                                 filesystem::u8path(targetSettingsPath.filename().u8string() + ".restore-journal");
      assert(!filesystem::exists(targetJournal));
      for (const auto& entry : filesystem::directory_iterator(targetSettingsPath.parent_path())) {
        assert(entry.path().filename().u8string().find(targetSettingsPath.filename().u8string() + ".restore-old-") != 0);
      }
      InventoryStore stillProtected;
      assert(stillProtected.load(restoreTarget / "inventory.db"));
      assert(stillProtected.items().front().id == "transfer-item");
    }
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
    assert(!filesystem::exists(restoreTarget / "manifest.tsv"));
    assert(!filesystem::exists(restoreTarget / "settings.conf"));

    {
      // Restore replaces only Inventatory's managed files. Everything else in
      // the chosen data folder is user data and must survive every outcome:
      // success, refusal before activation, a failed move, and a crash at any
      // step followed by startup recovery.
      const auto tempRoot = filesystem::temp_directory_path();
      const auto keepTarget = tempRoot / "inventatory-transfer-preserve-target";
      const auto keepSettings = tempRoot / "inventatory-transfer-preserve-settings.conf";
      const auto keepJournal = tempRoot / "inventatory-transfer-preserve-settings.conf.restore-journal";
      const auto keepOutsideFile = tempRoot / "inventatory-transfer-preserve-outside.txt";
      const auto keepOutsideDir = tempRoot / "inventatory-transfer-preserve-outside-dir";
      const string targetName = keepTarget.filename().u8string();
      const string settingsName = keepSettings.filename().u8string();
      const auto writeBytes = [](const filesystem::path& path, const string& bytes) {
        filesystem::create_directories(path.parent_path());
        ofstream output(path, ios::binary | ios::trunc);
        output.write(bytes.data(), static_cast<streamsize>(bytes.size()));
      };
      const auto readBytes = [](const filesystem::path& path) {
        ifstream input(path, ios::binary);
        return string(istreambuf_iterator<char>(input), istreambuf_iterator<char>());
      };
      const map<string, string> unmanagedFiles = {
          {"notes.txt", "datasheet notes\n"},
          {"sub/x.bin", string("\0\1\2\xff binary", 11)},
          {"sub/deeper/y.txt", "nested file"},
          {"Datasheets/part.pdf", "%PDF-1.4 not really"},
          {"inventory.db.bak", "a hand-made backup that only looks managed"},
          {"inventory-export.csv", "id,name\n1,thing\n"}};
      // Full, non-following picture of a tree so "unchanged" means unchanged.
      const auto snapshotTree = [&readBytes](const filesystem::path& root) {
        map<string, string> tree;
        for (filesystem::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
          const auto relative = filesystem::relative(iterator->path(), root).generic_u8string();
          const auto status = iterator->symlink_status();
          if (status.type() == filesystem::file_type::symlink) {
            tree[relative] = "L:" + filesystem::read_symlink(iterator->path()).generic_u8string();
          } else if (status.type() == filesystem::file_type::directory) {
            tree[relative] = "D";
          } else {
            tree[relative] = "F:" + readBytes(iterator->path());
          }
        }
        return tree;
      };
      const auto restoreArtifacts = [&]() {
        vector<string> artifacts;
        for (const auto& entry : filesystem::directory_iterator(tempRoot)) {
          const string name = entry.path().filename().u8string();
          if (name.find(".tmp-") != string::npos || name.find(".restore-copy-") != string::npos) continue;
          if (name.rfind(targetName + ".restore-", 0) == 0 || name.rfind(settingsName + ".restore-", 0) == 0) {
            artifacts.push_back(name);
          }
        }
        return artifacts;
      };
      const auto resetWorkspace = [&]() {
        error_code resetError;
        filesystem::remove_all(keepTarget, resetError);
        filesystem::remove_all(keepOutsideDir, resetError);
        filesystem::remove(keepOutsideFile, resetError);
        filesystem::remove(keepSettings, resetError);
        for (const auto& name : restoreArtifacts()) filesystem::remove_all(tempRoot / filesystem::u8path(name), resetError);
        filesystem::remove(keepJournal, resetError);
        filesystem::create_directories(keepTarget);
        InventoryStore original;
        InventoryItem originalItem;
        originalItem.id = "preserve-original";
        originalItem.partName = "Original item";
        original.items().push_back(originalItem);
        assert(original.save(keepTarget / "inventory.db"));
        writeBytes(keepTarget / "inventatory_scan.conf", "old pairing identity");
        writeBytes(keepTarget / "inventatory-scan-replay.state", "old replay state");
        for (const auto& file : unmanagedFiles) writeBytes(keepTarget / filesystem::u8path(file.first), file.second);
        filesystem::create_directories(keepTarget / "empty-folder");
#ifndef _WIN32
        writeBytes(keepOutsideFile, "outside the data folder");
        writeBytes(keepOutsideDir / "kept.txt", "outside folder content");
        filesystem::create_symlink(keepOutsideFile, keepTarget / "link-to-file");
        filesystem::create_directory_symlink(keepOutsideDir, keepTarget / "link-to-folder");
#endif
        AppSettings keepAppSettings;
        keepAppSettings.dataDirectory = keepTarget;
        assert(saveAppSettings(keepSettings, keepAppSettings));
      };
      const auto expectUnmanagedIntact = [&]() {
        for (const auto& file : unmanagedFiles) {
          const auto path = keepTarget / filesystem::u8path(file.first);
          assert(filesystem::is_regular_file(path));
          assert(readBytes(path) == file.second);
        }
        assert(filesystem::is_directory(keepTarget / "empty-folder"));
#ifndef _WIN32
        assert(filesystem::is_symlink(keepTarget / "link-to-file"));
        assert(filesystem::read_symlink(keepTarget / "link-to-file") == keepOutsideFile);
        assert(filesystem::is_symlink(keepTarget / "link-to-folder"));
        assert(readBytes(keepOutsideFile) == "outside the data folder");
        assert(readBytes(keepOutsideDir / "kept.txt") == "outside folder content");
#endif
      };
      const auto loadItemId = [&]() {
        InventoryStore loaded;
        assert(loaded.load(keepTarget / "inventory.db"));
        assert(loaded.items().size() == 1);
        return loaded.items().front().id;
      };
      // Real operations that can be interleaved with injected crashes. Every
      // hook is counted so a crash can be placed before or after any step.
      struct CrashPlan {
        size_t operations = 0;
        size_t crashAt = 0;  // 1-based; 0 never crashes
        bool crashAfter = false;
        bool crashed = false;
      };
      const auto makeCrashHooks = [](CrashPlan& plan) {
        InventoryTransferTestHooks hooks;
        const auto step = [&plan](const function<bool()>& action) {
          const size_t index = ++plan.operations;
          const bool crashHere = plan.crashAt == index;
          if (crashHere && !plan.crashAfter) {
            plan.crashed = true;
            throw runtime_error("injected crash");
          }
          const bool ok = action();
          if (crashHere) {
            plan.crashed = true;
            throw runtime_error("injected crash");
          }
          return ok;
        };
        hooks.copyFile = [step](const filesystem::path& from, const filesystem::path& to, string& injected) {
          return step([&]() {
            error_code ec;
            filesystem::copy_file(from, to, filesystem::copy_options::overwrite_existing, ec);
            if (ec) injected = ec.message();
            return !ec;
          });
        };
        hooks.renamePath = [step](const filesystem::path& from, const filesystem::path& to, string& injected) {
          return step([&]() {
            error_code ec;
            filesystem::rename(from, to, ec);
            if (ec) injected = ec.message();
            return !ec;
          });
        };
        hooks.removeAll = [step](const filesystem::path& path, string& injected) {
          return step([&]() {
            error_code ec;
            filesystem::remove_all(path, ec);
            if (ec) injected = ec.message();
            return !ec;
          });
        };
        hooks.replaceFile = [step](const filesystem::path& from, const filesystem::path& to, string& injected) {
          return step([&]() {
            error_code ec;
            filesystem::rename(from, to, ec);
            if (ec) injected = ec.message();
            return !ec;
          });
        };
        return hooks;
      };

      // 1. Success: unmanaged entries are untouched, managed files are the
      //    backup's, and workspace-bound state of the old data is gone.
      resetWorkspace();
      assert(restoreInventatoryBackup(bundle, keepTarget, keepSettings, error));
      expectUnmanagedIntact();
      assert(loadItemId() == "transfer-item");
      assert(!filesystem::exists(keepTarget / "inventatory_scan.conf"));
      assert(!filesystem::exists(keepTarget / "inventatory-scan-replay.state"));
      assert(!filesystem::exists(keepTarget / "manifest.tsv"));
      assert(!filesystem::exists(keepTarget / "settings.conf"));
      assert(restoreArtifacts().empty());

      // 2. Names that collide with managed or bundle files are refused before
      //    anything is moved, and nothing is deleted.
      const vector<pair<string, bool>> conflicts = {
          {"Inventory.DB", false}, {"manifest.tsv", false}, {"settings.conf", false}, {"activity.tsv", true}};
      for (const auto& conflict : conflicts) {
        resetWorkspace();
        error_code conflictError;
        filesystem::remove(keepTarget / "inventatory-scan-replay.state", conflictError);
        if (conflict.second) {
          filesystem::create_directories(keepTarget / filesystem::u8path(conflict.first));
          writeBytes(keepTarget / filesystem::u8path(conflict.first) / "inside.txt", "folder content");
        } else {
          writeBytes(keepTarget / filesystem::u8path(conflict.first), "user file");
        }
        const auto before = snapshotTree(keepTarget);
        bool replacementActive = true;
        assert(!restoreInventatoryBackup(bundle, keepTarget, keepSettings, error, nullptr, &replacementActive));
        assert(!replacementActive);
        assert(error.find(conflict.first) != string::npos);
        assert(snapshotTree(keepTarget) == before);
        assert(restoreArtifacts().empty());
        assert(!filesystem::exists(keepJournal));
        assert(loadItemId() == "preserve-original");
      }

      // 3. A failed move of any unmanaged entry rolls back completely.
      for (size_t failingMove = 0; failingMove < 4; ++failingMove) {
        resetWorkspace();
        const auto before = snapshotTree(keepTarget);
        size_t seenMoves = 0;
        InventoryTransferTestHooks hooks;
        hooks.renamePath = [&](const filesystem::path& from, const filesystem::path& to, string& injected) {
          if (from.parent_path().filename().u8string().find(".restore-old-data-") != string::npos &&
              seenMoves++ == failingMove) {
            injected = "injected move failure";
            return false;
          }
          error_code ec;
          filesystem::rename(from, to, ec);
          if (ec) injected = ec.message();
          return !ec;
        };
        bool replacementActive = true;
        assert(!restoreInventatoryBackup(bundle, keepTarget, keepSettings, error, &hooks, &replacementActive));
        assert(seenMoves > failingMove);
        assert(!replacementActive);
        assert(snapshotTree(keepTarget) == before);
        assert(restoreArtifacts().empty());
        assert(!filesystem::exists(keepJournal));
      }

      // 4. A crash before or after any single step leaves a state that startup
      //    recovery resolves without losing a byte of unmanaged data.
      resetWorkspace();
      CrashPlan countingPlan;
      {
        auto hooks = makeCrashHooks(countingPlan);
        assert(restoreInventatoryBackup(bundle, keepTarget, keepSettings, error, &hooks));
      }
      assert(countingPlan.operations > 20);
      size_t committedRecoveries = 0;
      size_t rolledBackRecoveries = 0;
      for (size_t crashAt = 1; crashAt <= countingPlan.operations; ++crashAt) {
        for (const bool crashAfter : {false, true}) {
          resetWorkspace();
          CrashPlan plan;
          plan.crashAt = crashAt;
          plan.crashAfter = crashAfter;
          auto hooks = makeCrashHooks(plan);
          assert(!restoreInventatoryBackup(bundle, keepTarget, keepSettings, error, &hooks));
          assert(plan.crashed);
          string recoveryError;
          const bool recovered = recoverInventatoryRestore(keepTarget, keepSettings, recoveryError);
          if (!recovered) {
            cerr << "recovery failed at step " << crashAt << (crashAfter ? " (after)" : " (before)") << ": "
                 << recoveryError << '\n';
          }
          assert(recovered);
          expectUnmanagedIntact();
          if (crashAt == 1 && !crashAfter) {
            // Crashing before the journal exists leaves only the empty staging
            // directory, which holds no data and has nothing to recover.
            for (const auto& name : restoreArtifacts()) {
              assert(name.find(".restore-staging-") != string::npos);
              assert(filesystem::is_empty(tempRoot / filesystem::u8path(name)));
            }
          } else {
            assert(restoreArtifacts().empty());
          }
          assert(!filesystem::exists(keepJournal));
          assert(!filesystem::exists(keepTarget / "manifest.tsv"));
          const auto itemId = loadItemId();
          assert(itemId == "preserve-original" || itemId == "transfer-item");
          if (itemId == "transfer-item") {
            ++committedRecoveries;
            assert(!filesystem::exists(keepTarget / "inventatory_scan.conf"));
            assert(!filesystem::exists(keepTarget / "inventatory-scan-replay.state"));
          } else {
            ++rolledBackRecoveries;
            assert(readBytes(keepTarget / "inventatory_scan.conf") == "old pairing identity");
          }
        }
      }
      assert(committedRecoveries > 0 && rolledBackRecoveries > 0);

      // 5. Committed cleanup never deletes an unmanaged entry that is still in
      //    the protected old data (for example, from an interrupted older
      //    release): it is moved into the active workspace first, and a name
      //    clash leaves the protected data in place instead of deleting it.
      const auto crashAtCleanupCommit = [&](const string& leftoverName) {
        resetWorkspace();
        InventoryTransferTestHooks hooks;
        hooks.replaceFile = [&](const filesystem::path& from, const filesystem::path& to, string& injected) {
          if (to == keepJournal && readBytes(from).find("cleanup_pending") != string::npos) {
            for (const auto& entry : filesystem::directory_iterator(tempRoot)) {
              if (entry.path().filename().u8string().rfind(targetName + ".restore-old-data-", 0) == 0) {
                writeBytes(entry.path() / filesystem::u8path(leftoverName), "left in protected data");
              }
            }
            throw runtime_error("injected crash");
          }
          error_code ec;
          filesystem::rename(from, to, ec);
          if (ec) injected = ec.message();
          return !ec;
        };
        assert(!restoreInventatoryBackup(bundle, keepTarget, keepSettings, error, &hooks));
        assert(filesystem::exists(keepJournal));
      };
      crashAtCleanupCommit("late.txt");
      assert(recoverInventatoryRestore(keepTarget, keepSettings, error));
      expectUnmanagedIntact();
      assert(readBytes(keepTarget / "late.txt") == "left in protected data");
      assert(loadItemId() == "transfer-item");
      assert(restoreArtifacts().empty());
      crashAtCleanupCommit("notes.txt");
      assert(!recoverInventatoryRestore(keepTarget, keepSettings, error));
      assert(error.find("notes.txt") != string::npos);
      assert(filesystem::exists(keepJournal));
      expectUnmanagedIntact();
      bool leftoverKept = false;
      for (const auto& entry : filesystem::directory_iterator(tempRoot)) {
        if (entry.path().filename().u8string().rfind(targetName + ".restore-old-data-", 0) == 0) {
          assert(readBytes(entry.path() / "notes.txt") == "left in protected data");
          leftoverKept = true;
        }
      }
      assert(leftoverKept);

      error_code keepCleanupError;
      filesystem::remove_all(keepTarget, keepCleanupError);
      filesystem::remove_all(keepOutsideDir, keepCleanupError);
      filesystem::remove(keepOutsideFile, keepCleanupError);
      filesystem::remove(keepSettings, keepCleanupError);
      filesystem::remove(keepJournal, keepCleanupError);
      for (const auto& name : restoreArtifacts()) filesystem::remove_all(tempRoot / filesystem::u8path(name), keepCleanupError);
    }

    // If the protected old directory disappears before rollback, the active
    // destination must remain intact rather than being deleted blindly.
    const auto missingOldTarget = filesystem::temp_directory_path() / "inventatory-transfer-missing-old-target";
    const auto missingOldSettings = filesystem::temp_directory_path() / "inventatory-transfer-missing-old-settings.conf";
    filesystem::remove_all(missingOldTarget, cleanupError);
    filesystem::remove(missingOldSettings, cleanupError);
    filesystem::create_directories(missingOldTarget);
    InventoryStore missingOldProtected;
    InventoryItem missingOldItem;
    missingOldItem.id = "protected-missing-old";
    missingOldItem.partName = "Protected missing-old item";
    missingOldProtected.items().push_back(missingOldItem);
    assert(missingOldProtected.save(missingOldTarget / "inventory.db"));
    AppSettings missingOldAppSettings;
    missingOldAppSettings.dataDirectory = missingOldTarget;
    assert(saveAppSettings(missingOldSettings, missingOldAppSettings));
    InventoryTransferTestHooks missingOldHooks;
    missingOldHooks.saveSettings = [missingOldTarget](const filesystem::path&, const AppSettings&, string& injectedError) {
      error_code removeError;
      for (const auto& entry : filesystem::directory_iterator(missingOldTarget.parent_path(), removeError)) {
        if (removeError) break;
        if (entry.path().filename().u8string().find(missingOldTarget.filename().u8string() + ".restore-old-data-") == 0) {
          filesystem::remove_all(entry.path(), removeError);
          break;
        }
      }
      injectedError = "injected settings activation failure after old-data loss";
      return false;
    };
    assert(!restoreInventatoryBackup(bundle, missingOldTarget, missingOldSettings, error, &missingOldHooks));
    InventoryStore stillActiveAfterLostRollback;
    assert(stillActiveAfterLostRollback.load(missingOldTarget / "inventory.db"));
    assert(stillActiveAfterLostRollback.items().front().id == "transfer-item");
    assert(!recoverInventatoryRestore(missingOldTarget, missingOldSettings, error));
    filesystem::remove_all(missingOldTarget, cleanupError);
    filesystem::remove(missingOldSettings, cleanupError);
    filesystem::remove(missingOldSettings.parent_path() /
                           filesystem::u8path(missingOldSettings.filename().u8string() + ".restore-journal"),
                       cleanupError);

    const auto preparedTarget = filesystem::temp_directory_path() / "inventatory-transfer-prepared-target";
    const auto preparedSettings = filesystem::temp_directory_path() / "inventatory-transfer-prepared-settings.conf";
    filesystem::remove_all(preparedTarget, cleanupError);
    filesystem::remove(preparedSettings, cleanupError);
    filesystem::create_directories(preparedTarget);
    InventoryStore preparedStore;
    InventoryItem preparedItem;
    preparedItem.id = "prepared-protected";
    preparedItem.partName = "Prepared protected item";
    preparedStore.items().push_back(preparedItem);
    assert(preparedStore.save(preparedTarget / "inventory.db"));
    AppSettings preparedAppSettings;
    preparedAppSettings.dataDirectory = preparedTarget;
    assert(saveAppSettings(preparedSettings, preparedAppSettings));
    InventoryTransferTestHooks preparedHooks;
    preparedHooks.copyFile = [](const filesystem::path&, const filesystem::path&, string& injectedError) {
      injectedError = "injected staging copy failure";
      return false;
    };
    preparedHooks.removeAll = [](const filesystem::path& path, string& injectedError) {
      if (path.filename().u8string().find(".restore-staging-") != string::npos) {
        injectedError = "injected staging cleanup failure";
        return false;
      }
      error_code removeError;
      filesystem::remove_all(path, removeError);
      if (removeError) {
        injectedError = removeError.message();
        return false;
      }
      return true;
    };
    assert(!restoreInventatoryBackup(bundle, preparedTarget, preparedSettings, error, &preparedHooks));
    InventoryStore preparedStillProtected;
    assert(preparedStillProtected.load(preparedTarget / "inventory.db"));
    assert(preparedStillProtected.items().front().id == "prepared-protected");
    assert(recoverInventatoryRestore(preparedTarget, preparedSettings, error));
    filesystem::remove_all(preparedTarget, cleanupError);
    filesystem::remove(preparedSettings, cleanupError);

    // Online backup must capture the last committed WAL state without waiting
    // for or including a writer's uncommitted transaction.
    const auto liveBundle = filesystem::temp_directory_path() / "inventatory-transfer-live-bundle";
    filesystem::remove_all(liveBundle, cleanupError);
    SqliteConnection liveConnection;
    assert(openDatabase(source / "inventory.db", liveConnection));
    assert(execSql(liveConnection, "PRAGMA journal_mode=WAL"));
    assert(execSql(liveConnection, "BEGIN IMMEDIATE"));
    assert(execSql(liveConnection, "UPDATE inventatory_items SET quantity=999 WHERE id='transfer-item'"));
    assert(createInventatoryBackup(source, settingsPath, liveBundle, "1.0.0", error));
    InventoryStore liveSnapshot;
    assert(liveSnapshot.load(liveBundle / "inventory.db"));
    assert(liveSnapshot.items().front().quantity == 13);
    assert(execSql(liveConnection, "ROLLBACK"));
    filesystem::remove_all(liveBundle, cleanupError);

    filesystem::remove_all(source, cleanupError);
    filesystem::remove_all(bundle, cleanupError);
    filesystem::remove_all(restoreTarget, cleanupError);
    filesystem::remove(settingsPath, cleanupError);
    filesystem::remove(targetSettingsPath, cleanupError);
    filesystem::remove(csv, cleanupError);
  }

  {
    const auto root = filesystem::temp_directory_path();
    const auto emptySource = root / "inventatory-transfer-empty-source";
    const auto emptyBundle = root / "inventatory-transfer-empty-bundle";
    const auto noSchemaSource = root / "inventatory-transfer-no-schema-source";
    const auto noSchemaBundle = root / "inventatory-transfer-no-schema-bundle";
    const auto invalidBundle = root / "inventatory-transfer-invalid-bundle";
    const auto foreignBundle = root / "inventatory-transfer-foreign-bundle";
    const auto malformedBundle = root / "inventatory-transfer-malformed-bundle";
    const auto missingBundle = root / "inventatory-transfer-missing-bundle";
    const auto unicodeSource = root / filesystem::u8path("inventatory-transfer-źródło");
    const auto unicodeBundle = root / filesystem::u8path("inventatory-transfer-kopia-保存");
    const auto emptySettingsPath = root / "inventatory-transfer-empty-settings.conf";
    const auto noSchemaSettingsPath = root / "inventatory-transfer-no-schema-settings.conf";
    const auto unicodeSettingsPath = root / filesystem::u8path("inventatory-transfer-ustawienia-保存.conf");
    error_code cleanupError;
    const vector<filesystem::path> cleanupPaths = {emptySource,       emptyBundle,       noSchemaSource,
                                                   noSchemaBundle,     invalidBundle,     foreignBundle,
                                                   malformedBundle,    missingBundle,     unicodeSource,
                                                   unicodeBundle,      emptySettingsPath, noSchemaSettingsPath,
                                                   unicodeSettingsPath};
    for (const auto& path : cleanupPaths) filesystem::remove_all(path, cleanupError);
    filesystem::create_directories(emptySource);
    InventoryStore emptyStore;
    assert(emptyStore.save(emptySource / "inventory.db"));
    AppSettings emptySettings;
    emptySettings.dataDirectory = emptySource;
    emptySettings.completedOnboardingVersion = 1;
    assert(saveAppSettings(emptySettingsPath, emptySettings));
    string error;
    const auto nestedDestination = emptySource / "unsafe-backup";
    assert(!createInventatoryBackup(emptySource, emptySettingsPath, nestedDestination, "1.0.0", error));
    assert(createInventatoryBackup(emptySource, emptySettingsPath, emptyBundle, "1.0.0", error));
    assert(validateInventatoryBackup(emptyBundle, error));
    assert(!restoreInventatoryBackup(emptyBundle, emptyBundle, emptySettingsPath, error));

    // A readable SQLite file without the Inventatory schema is rejected by
    // the staged public backup workflow, and the source is not modified.
    filesystem::create_directories(noSchemaSource);
    {
      SqliteConnection noSchemaConnection;
      assert(openDatabase(noSchemaSource / "inventory.db", noSchemaConnection));
      assert(execSql(noSchemaConnection, "CREATE TABLE foreign_table(value TEXT)"));
    }
    AppSettings noSchemaSettings;
    noSchemaSettings.dataDirectory = noSchemaSource;
    noSchemaSettings.completedOnboardingVersion = 1;
    assert(saveAppSettings(noSchemaSettingsPath, noSchemaSettings));
    ifstream noSchemaBefore(noSchemaSource / "inventory.db", ios::binary);
    const string noSchemaBytesBefore((istreambuf_iterator<char>(noSchemaBefore)), istreambuf_iterator<char>());
    assert(!createInventatoryBackup(noSchemaSource, noSchemaSettingsPath, noSchemaBundle, "1.0.0", error));
    ifstream noSchemaAfter(noSchemaSource / "inventory.db", ios::binary);
    const string noSchemaBytesAfter((istreambuf_iterator<char>(noSchemaAfter)), istreambuf_iterator<char>());
    assert(noSchemaBytesBefore == noSchemaBytesAfter);
    assert(!filesystem::exists(noSchemaBundle));

    const auto rewriteInventoryManifest = [](const filesystem::path& bundle, uintmax_t size, const string& hash) {
      ifstream input(bundle / "manifest.tsv", ios::binary);
      const string original((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
      const string prefix = "file\tinventory.db\t";
      const auto start = original.find(prefix);
      if (start == string::npos) return false;
      const auto end = original.find('\n', start);
      const string replacement = prefix + to_string(size) + '\t' + hash;
      string rewritten = original.substr(0, start) + replacement;
      if (end != string::npos) rewritten += original.substr(end);
      ofstream output(bundle / "manifest.tsv", ios::binary | ios::trunc);
      output << rewritten;
      output.close();
      return static_cast<bool>(output);
    };
    const string emptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const string foreignHash = "656771905e1ef731f65cd0a0d9fb061238380a1a012e6abdf846ecc7d2ea36fd";
    filesystem::copy(emptyBundle, invalidBundle, filesystem::copy_options::recursive, cleanupError);
    assert(!cleanupError);
    ofstream zeroDatabase(invalidBundle / "inventory.db", ios::binary | ios::trunc);
    zeroDatabase.close();
    assert(rewriteInventoryManifest(invalidBundle, 0, emptyHash));
    ifstream zeroBefore(invalidBundle / "inventory.db", ios::binary);
    const string zeroBytesBefore((istreambuf_iterator<char>(zeroBefore)), istreambuf_iterator<char>());
    assert(!validateInventatoryBackup(invalidBundle, error));
    ifstream zeroAfter(invalidBundle / "inventory.db", ios::binary);
    const string zeroBytesAfter((istreambuf_iterator<char>(zeroAfter)), istreambuf_iterator<char>());
    assert(zeroBytesBefore == zeroBytesAfter);

    filesystem::copy(emptyBundle, foreignBundle, filesystem::copy_options::recursive, cleanupError);
    assert(!cleanupError);
    ofstream foreignDatabase(foreignBundle / "inventory.db", ios::binary | ios::trunc);
    foreignDatabase << "foreign";
    foreignDatabase.close();
    assert(rewriteInventoryManifest(foreignBundle, 7, foreignHash));
    ifstream foreignBefore(foreignBundle / "inventory.db", ios::binary);
    const string foreignBytesBefore((istreambuf_iterator<char>(foreignBefore)), istreambuf_iterator<char>());
    assert(!validateInventatoryBackup(foreignBundle, error));
    ifstream foreignAfter(foreignBundle / "inventory.db", ios::binary);
    const string foreignBytesAfter((istreambuf_iterator<char>(foreignAfter)), istreambuf_iterator<char>());
    assert(foreignBytesBefore == foreignBytesAfter);

    filesystem::copy(emptyBundle, malformedBundle, filesystem::copy_options::recursive, cleanupError);
    assert(!cleanupError);
    ifstream manifestInput(malformedBundle / "manifest.tsv", ios::binary);
    const string validManifest((istreambuf_iterator<char>(manifestInput)), istreambuf_iterator<char>());
    const vector<string> badRows = {"inventatory_backup_format\t1\n", "application_version\t1.0.0\n",
                                    "file\tactivity.tsv\tnot-a-size\t" + string(64, '0') + "\n",
                                    "file\tinventory.db\t0\tbad\n", "file\tinventory.db\t0\t" + emptyHash + "\n"};
    for (const auto& badRow : badRows) {
      ofstream output(malformedBundle / "manifest.tsv", ios::binary | ios::trunc);
      output << validManifest << badRow;
      output.close();
      assert(!validateInventatoryBackup(malformedBundle, error));
    }
    ofstream restoreManifest(malformedBundle / "manifest.tsv", ios::binary | ios::trunc);
    restoreManifest << validManifest;
    restoreManifest.close();
    string uppercaseManifest = validManifest;
    size_t lineStart = 0;
    while (lineStart < uppercaseManifest.size()) {
      const auto lineEnd = uppercaseManifest.find('\n', lineStart);
      const auto end = lineEnd == string::npos ? uppercaseManifest.size() : lineEnd;
      if (uppercaseManifest.compare(lineStart, 5, "file\t") == 0) {
        const auto hashStart = uppercaseManifest.rfind('\t', end == 0 ? 0 : end - 1);
        if (hashStart != string::npos && hashStart >= lineStart) {
          transform(uppercaseManifest.begin() + static_cast<ptrdiff_t>(hashStart + 1),
                    uppercaseManifest.begin() + static_cast<ptrdiff_t>(end), uppercaseManifest.begin() +
                    static_cast<ptrdiff_t>(hashStart + 1),
                    [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
        }
      }
      if (lineEnd == string::npos) break;
      lineStart = lineEnd + 1;
    }
    ofstream uppercaseOutput(malformedBundle / "manifest.tsv", ios::binary | ios::trunc);
    uppercaseOutput << uppercaseManifest;
    uppercaseOutput.close();
    assert(validateInventatoryBackup(malformedBundle, error));
    ofstream restoreManifestAgain(malformedBundle / "manifest.tsv", ios::binary | ios::trunc);
    restoreManifestAgain << validManifest;
    restoreManifestAgain.close();
    filesystem::copy(emptyBundle, missingBundle, filesystem::copy_options::recursive, cleanupError);
    assert(!cleanupError);
    filesystem::remove(missingBundle / "settings.conf", cleanupError);
    assert(!validateInventatoryBackup(missingBundle, error));

    ofstream secret(emptySource / "inventatory_scan.conf", ios::binary);
    secret << "token=must-not-leak";
    secret.close();
    const auto secretBundle = root / "inventatory-transfer-secret-bundle";
    filesystem::remove_all(secretBundle, cleanupError);
    assert(createInventatoryBackup(emptySource, emptySettingsPath, secretBundle, "1.0.0", error));
    assert(!filesystem::exists(secretBundle / "inventatory_scan.conf"));
    InventoryTransferTestHooks copyFailureHooks;
    copyFailureHooks.copyFile = [](const filesystem::path&, const filesystem::path&, string& injectedError) {
      injectedError = "injected copy failure";
      return false;
    };
    const auto failedBundle = root / "inventatory-transfer-failed-bundle";
    filesystem::remove_all(failedBundle, cleanupError);
    ofstream optionalFile(emptySource / "activity.tsv", ios::binary);
    optionalFile << "activity";
    optionalFile.close();
    assert(!createInventatoryBackup(emptySource, emptySettingsPath, failedBundle, "1.0.0", error,
                                    &copyFailureHooks));
    assert(!filesystem::exists(failedBundle));

    filesystem::create_directories(unicodeSource);
    InventoryStore unicodeStore;
    assert(unicodeStore.save(unicodeSource / "inventory.db"));
    AppSettings unicodeSettings;
    unicodeSettings.dataDirectory = unicodeSource;
    unicodeSettings.completedOnboardingVersion = 1;
    assert(saveAppSettings(unicodeSettingsPath, unicodeSettings));
    assert(createInventatoryBackup(unicodeSource, unicodeSettingsPath, unicodeBundle, "1.0.0", error));
    assert(validateInventatoryBackup(unicodeBundle, error));

    for (const auto& path : cleanupPaths) filesystem::remove_all(path, cleanupError);
    filesystem::remove_all(secretBundle, cleanupError);
    filesystem::remove_all(failedBundle, cleanupError);
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
    assert(pending.front().deviceId == "r1-recovery");

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

  {
    error_code cleanupError;
    const auto tempDir = filesystem::temp_directory_path() / ("inventatory-test-createdir-" + to_string(time(nullptr)));
    filesystem::remove_all(tempDir, cleanupError);
    const auto dbPath = tempDir / "nested" / "subfolder" / "inventory.db";
    assert(!filesystem::exists(dbPath.parent_path()));

    {
      SqliteConnection connection;
      assert(openDatabase(dbPath, connection));
      assert(connection.db != nullptr);
      assert(filesystem::is_directory(dbPath.parent_path()));
      assert(filesystem::is_regular_file(dbPath));
      assert(ensureInventoryDatabaseSchema(connection));
    }

    InventoryStore current;
    assert(ensureInventoryCommitHistory(dbPath, current));
    vector<InventoryCommit> commits;
    assert(loadInventoryCommits(dbPath, commits));
    assert(commits.size() == 1);
    assert(commits.front().message == "Initial inventory");

    filesystem::remove_all(tempDir, cleanupError);
  }

#ifndef _WIN32
  {
    // Private runtime directory: a UI or service the developer has open must not hold these locks.
    const auto runtimeRoot = filesystem::temp_directory_path() / ("inventatory-signal-test-" + to_string(getpid()));
    filesystem::create_directories(runtimeRoot);
    filesystem::permissions(runtimeRoot, filesystem::perms::owner_all, filesystem::perm_options::replace);
    const char* previousRuntime = getenv("XDG_RUNTIME_DIR");
    const string previousRuntimeCopy = previousRuntime != nullptr ? previousRuntime : "";
    setenv("XDG_RUNTIME_DIR", runtimeRoot.c_str(), 1);
    BackgroundController controller;
    assert(controller.acquireSingleInstance(false));
    bool opened = false;
    assert(controller.start(false, false, [] {}, [&opened] { opened = true; }));
    assert(controller.signalExistingInstance());
    this_thread::sleep_for(chrono::milliseconds(100));
    assert(opened);
    controller.stop();
    if (previousRuntime != nullptr) setenv("XDG_RUNTIME_DIR", previousRuntimeCopy.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    error_code signalCleanupError;
    filesystem::remove_all(runtimeRoot, signalCleanupError);
  }

  {
    // Background-service takeover, in a private runtime directory so no real service is touched.
    const auto runtimeRoot = filesystem::temp_directory_path() / ("inventatory-takeover-test-" + to_string(getpid()));
    filesystem::create_directories(runtimeRoot);
    filesystem::permissions(runtimeRoot, filesystem::perms::owner_all, filesystem::perm_options::replace);
    const char* previousRuntime = getenv("XDG_RUNTIME_DIR");
    const string previousRuntimeCopy = previousRuntime != nullptr ? previousRuntime : "";
    setenv("XDG_RUNTIME_DIR", runtimeRoot.c_str(), 1);

    {
      BackgroundController none;
      assert(!none.backgroundServiceRunning());
      assert(none.stopBackgroundService(200, 200) == BackgroundStopResult::NotRunning);
    }

    // Starts a child that owns the background lock. A cooperative child quits on SIGTERM; a stubborn
    // one ignores it, standing in for a service stuck in its shutdown.
    const auto spawnService = [&](bool stubborn) {
      int ready[2]{};
      assert(pipe(ready) == 0);
      const pid_t child = fork();
      assert(child >= 0);
      if (child == 0) {
        close(ready[0]);
        BackgroundController service;
        if (!service.acquireSingleInstance(true)) _exit(2);
        atomic<bool> quit{false};
        if (!service.start(true, true, [&quit] { quit.store(true); })) _exit(3);
        if (stubborn) signal(SIGTERM, SIG_IGN);
        const char go = 1;
        if (write(ready[1], &go, 1) != 1) _exit(4);
        close(ready[1]);
        for (int tick = 0; tick < 3000 && (stubborn || !quit.load()); ++tick) {
          this_thread::sleep_for(chrono::milliseconds(10));
        }
        service.stop();
        _exit(stubborn ? 5 : 0);
      }
      close(ready[1]);
      char go = 0;
      assert(read(ready[0], &go, 1) == 1);
      close(ready[0]);
      return child;
    };

    {
      const pid_t child = spawnService(false);
      BackgroundController launcher;
      assert(launcher.backgroundServiceRunning());
      assert(launcher.stopBackgroundService(5000, 1000) == BackgroundStopResult::Stopped);
      int status = 0;
      assert(waitpid(child, &status, 0) == child);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
      assert(!launcher.backgroundServiceRunning());
    }
    {
      const pid_t child = spawnService(true);
      BackgroundController launcher;
      int reported = 0;
      assert(launcher.stopBackgroundService(1500, 2000, [&reported](int) { ++reported; }) ==
             BackgroundStopResult::Forced);
      assert(reported >= 1);
      int status = 0;
      assert(waitpid(child, &status, 0) == child);
      assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
      assert(!launcher.backgroundServiceRunning());
    }
    {
      // A stale PID that belongs to some other program must never be signalled.
      const auto lockFile = runtimeRoot / "inventatory" / "background.lock";
      const int descriptor = open(lockFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      assert(descriptor >= 0);
      assert(flock(descriptor, LOCK_EX | LOCK_NB) == 0);
      const pid_t sleeper = fork();
      assert(sleeper >= 0);
      if (sleeper == 0) {
        close(descriptor);
        execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
      }
      // Until it has exec'd, the child still is a copy of this test program.
      for (int tick = 0; tick < 200; ++tick) {
        error_code linkError;
        if (filesystem::read_symlink("/proc/" + to_string(sleeper) + "/exe", linkError).filename() != filesystem::read_symlink("/proc/self/exe").filename()) break;
        this_thread::sleep_for(chrono::milliseconds(10));
      }
      const auto text = to_string(sleeper) + "\n";
      assert(ftruncate(descriptor, 0) == 0 && write(descriptor, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
      BackgroundController launcher;
      assert(launcher.backgroundServiceRunning());
      assert(!launcher.requestBackgroundServiceQuit());
      assert(!launcher.forceStopBackgroundService());
      assert(kill(sleeper, 0) == 0);
      kill(sleeper, SIGKILL);
      waitpid(sleeper, nullptr, 0);
      close(descriptor);
    }
    {
      // Closing the terminal sends SIGHUP; it must reach the quit callback instead of killing the
      // process before its final save.
      BackgroundController controller;
      assert(controller.acquireSingleInstance(false));
      atomic<bool> quit{false};
      assert(controller.start(false, false, [&quit] { quit.store(true); }));
      assert(raise(SIGHUP) == 0);
      for (int tick = 0; tick < 100 && !quit.load(); ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(quit.load());
      controller.stop();
    }
    {
      // Turning "run in background" off in Settings only ends background mode: the running TUI keeps
      // its single-instance lock and its graceful-shutdown signal handling until the process exits.
      const auto handlerOf = [](int signalNumber) {
        struct sigaction current{};
        assert(sigaction(signalNumber, nullptr, &current) == 0);
        return current.sa_handler;
      };
      // A second launch is another process: it must still find the interactive instance and be
      // refused the workspace. (A forked child stands in for it and exits without running any
      // controller teardown, which would reset this process's signal handlers.)
      const auto secondLaunchBlocked = [] {
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
          BackgroundController second;
          if (!second.interactiveInstanceRunning()) _exit(2);
          _exit(second.acquireSingleInstance(false) ? 1 : 0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      };
      const auto hangupBefore = handlerOf(SIGHUP);
      const auto terminateBefore = handlerOf(SIGTERM);
      const auto interruptBefore = handlerOf(SIGINT);

      BackgroundController controller;
      assert(controller.acquireSingleInstance(false));
      atomic<int> quits{0};
      atomic<int> opens{0};
      assert(controller.start(true, false, [&quits] { ++quits; }, [&opens] { ++opens; }));
      assert(controller.enabled());

      controller.disableBackgroundMode();
      assert(!controller.enabled());
      controller.disableBackgroundMode();  // idempotent
      assert(secondLaunchBlocked());
      assert(handlerOf(SIGHUP) != hangupBefore);
      assert(handlerOf(SIGTERM) != terminateBefore);
      assert(handlerOf(SIGINT) != interruptBefore);
      // The bring-forward signal and terminal-close/kill still reach the original callbacks.
      assert(controller.signalExistingInstance());
      for (int tick = 0; tick < 100 && opens.load() == 0; ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(opens.load() == 1);
      assert(raise(SIGHUP) == 0);
      for (int tick = 0; tick < 100 && quits.load() == 0; ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(quits.load() == 1);
      assert(raise(SIGTERM) == 0);
      for (int tick = 0; tick < 100 && quits.load() == 1; ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(quits.load() == 2);

      // Re-enabling in the same session (as Settings does, without an open callback) keeps the open
      // callback and the instance lock, and does not start a second signal thread.
      atomic<int> laterQuits{0};
      assert(controller.start(true, false, [&laterQuits] { ++laterQuits; }));
      assert(controller.enabled());
      assert(controller.signalExistingInstance());
      for (int tick = 0; tick < 100 && opens.load() == 1; ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(opens.load() == 2);
      assert(raise(SIGHUP) == 0);
      for (int tick = 0; tick < 100 && laterQuits.load() == 0; ++tick) this_thread::sleep_for(chrono::milliseconds(10));
      assert(laterQuits.load() == 1);
      assert(secondLaunchBlocked());

      // Process shutdown still releases everything exactly once.
      controller.stop();
      controller.stop();
      assert(!controller.enabled());
      assert(handlerOf(SIGHUP) == hangupBefore);
      assert(handlerOf(SIGTERM) == terminateBefore);
      assert(handlerOf(SIGINT) == interruptBefore);
      BackgroundController next;
      assert(!next.interactiveInstanceRunning());
      assert(next.acquireSingleInstance(false));
      next.stop();
    }

    if (previousRuntime != nullptr) setenv("XDG_RUNTIME_DIR", previousRuntimeCopy.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    error_code runtimeCleanupError;
    filesystem::remove_all(runtimeRoot, runtimeCleanupError);
  }

  {
    // The per-user unit is rewritten and reloaded only when it changes, and keeps pointing at the
    // executable it was registered with.
    const auto root = filesystem::temp_directory_path() / ("inventatory-unit-test-" + to_string(getpid()));
    filesystem::create_directories(root / "bin");
    filesystem::create_directories(root / "config");
    const auto log = root / "systemctl.log";
    {
      ofstream shim(root / "bin" / "systemctl");
      shim << "#!/bin/sh\necho \"$*\" >> '" << log.string() << "'\nexit 0\n";
    }
    filesystem::permissions(root / "bin" / "systemctl", filesystem::perms::owner_all, filesystem::perm_options::replace);
    const string previousPath = getenv("PATH") != nullptr ? getenv("PATH") : "";
    const char* previousConfig = getenv("XDG_CONFIG_HOME");
    const string previousConfigCopy = previousConfig != nullptr ? previousConfig : "";
    setenv("PATH", ((root / "bin").string() + ":" + previousPath).c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);

    const auto countLines = [&log](const string& needle) {
      ifstream stream(log);
      string line;
      int count = 0;
      while (getline(stream, line)) count += line.find(needle) != string::npos ? 1 : 0;
      return count;
    };
    string unitError;
    const auto unit = root / "config" / "systemd" / "user" / "inventatory-background.service";
    assert(!startBackgroundServiceUnit(unitError));  // not registered yet
    assert(setBackgroundStartupEnabled(true, unitError));
    assert(filesystem::is_regular_file(unit));
    assert(countLines("daemon-reload") == 1 && countLines("enable") == 1);
    struct stat first{}, second{};
    assert(stat(unit.c_str(), &first) == 0);
    assert(setBackgroundStartupEnabled(true, unitError));
    assert(stat(unit.c_str(), &second) == 0);
    assert(first.st_ino == second.st_ino);
    assert(countLines("daemon-reload") == 1 && countLines("--user enable") == 1);

    ifstream unitStream(unit);
    const string unitText((istreambuf_iterator<char>(unitStream)), istreambuf_iterator<char>());
    assert(unitText.find("After=network-online.target") == string::npos);
    assert(unitText.find("TimeoutStopSec=15") != string::npos);
    assert(unitText.find("--background") != string::npos);

    // An existing registration that still resolves keeps its executable.
    {
      ofstream other(root / "other-binary");
      other << "#!/bin/sh\n";
    }
    filesystem::permissions(root / "other-binary", filesystem::perms::owner_all, filesystem::perm_options::replace);
    {
      ofstream edited(unit, ios::trunc);
      edited << "[Service]\nExecStart=\"" << (root / "other-binary").string() << "\" --background\n";
    }
    assert(setBackgroundStartupEnabled(true, unitError));
    ifstream keptStream(unit);
    const string keptText((istreambuf_iterator<char>(keptStream)), istreambuf_iterator<char>());
    assert(keptText.find((root / "other-binary").string()) != string::npos);

    assert(startBackgroundServiceUnit(unitError));
    assert(countLines("start inventatory-background.service") == 1);

    if (previousConfig != nullptr) setenv("XDG_CONFIG_HOME", previousConfigCopy.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    setenv("PATH", previousPath.c_str(), 1);
    error_code unitCleanupError;
    filesystem::remove_all(root, unitCleanupError);
  }
#endif

  testScannerHttpNegativeCases();
#ifndef _WIN32
  testScannerServerSurvivesSignals();
#endif

  testDeviceSyncRetryBackoff();
  testSettingsBridgeNotice();
  testScannerCredentialResolution();
  cout << "Inventatory core tests passed\n";
  return 0;
}
