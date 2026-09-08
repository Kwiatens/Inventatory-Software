// Inventatory - Inventory export and backup/restore workflows.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/storage/InventorySqlite.h"
#include "label_printer/core/LabelPrinter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

namespace inventatory {
using namespace std;

namespace {
string csvQuote(const string& value) {
  string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') escaped.push_back('"');
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

string rackLocationForExport(const InventoryItem& item, const InventoryStore& store) {
  const auto location = rackLocation(item, store.racks());
  return location.empty() ? item.location : location;
}
}  // namespace

using namespace inventory_transfer_detail;

bool exportInventoryCsv(const InventoryStore& store, const filesystem::path& path, string& error) {
  ofstream output(path, ios::binary | ios::trunc);
  if (!output) {
    error = "Unable to create " + path.string();
    return false;
  }

  output << "ID,Part name,Manufacturer,Category,Quantity,Location,SKU,Machine code,"
            "DigiKey part,Sync status,Tags,Parameters,Notes,Datasheet URL,Product URL,Last updated\r\n";
  for (const auto& item : store.items()) {
    output << csvQuote(item.id) << ',' << csvQuote(item.partName) << ',' << csvQuote(item.manufacturer) << ','
           << csvQuote(item.category) << ',' << item.quantity << ','
           << csvQuote(rackLocationForExport(item, store)) << ',' << csvQuote(item.sku) << ','
           << csvQuote(item.machineCode) << ',' << csvQuote(item.digikeyPartNumber) << ','
           << csvQuote(item.syncStatus) << ',' << csvQuote(join(item.tags, ';')) << ','
           << csvQuote(serializeParametersForStorage(item.parameters)) << ',' << csvQuote(item.notes) << ','
           << csvQuote(item.datasheetUrl) << ',' << csvQuote(item.productUrl) << ','
           << csvQuote(nowTimestampString(item.lastUpdated)) << "\r\n";
  }

  output.close();
  if (!output) {
    error = "Unable to finish writing " + path.string();
    return false;
  }
  return true;
}

}  // namespace inventatory
