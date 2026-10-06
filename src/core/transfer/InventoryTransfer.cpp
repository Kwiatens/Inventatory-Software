// Inventatory - Inventory export and backup/restore workflows.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/storage/AtomicFile.h"
#include "core/transfer/CsvExport.h"
#include "core/storage/InventorySqlite.h"
#include "label_printer/core/LabelPrinter.h"

#include <sstream>

#ifdef _WIN32
#include <windows.h>
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
  // Build the whole file first and replace the destination atomically, so a failed or interrupted
  // export never leaves a truncated file over the previous one.
  ostringstream output;
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

  string writeError;
  if (!writeFileAtomically(path, output.str(), &writeError)) {
    error = "Unable to write " + path.u8string() + (writeError.empty() ? string() : ": " + writeError);
    return false;
  }
  return true;
}

}  // namespace inventatory
