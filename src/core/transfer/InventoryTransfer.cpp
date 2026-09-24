// Inventatory - Inventory export and backup/restore workflows.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/transfer/CsvExport.h"
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
    output << csvTextCell(item.id) << ',' << csvTextCell(item.partName) << ',' << csvTextCell(item.manufacturer) << ','
           << csvTextCell(item.category) << ',' << item.quantity << ','
           << csvTextCell(rackLocationForExport(item, store)) << ',' << csvTextCell(item.sku) << ','
           << csvTextCell(item.machineCode) << ',' << csvTextCell(item.digikeyPartNumber) << ','
           << csvTextCell(item.syncStatus) << ',' << csvTextCell(join(item.tags, ';')) << ','
           << csvTextCell(serializeParametersForStorage(item.parameters)) << ',' << csvTextCell(item.notes) << ','
           << csvTextCell(item.datasheetUrl) << ',' << csvTextCell(item.productUrl) << ','
           << csvTextCell(nowTimestampString(item.lastUpdated)) << "\r\n";
  }

  output.close();
  if (!output) {
    error = "Unable to finish writing " + path.string();
    return false;
  }
  return true;
}

}  // namespace inventatory
