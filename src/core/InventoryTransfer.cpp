// Inventatory - Inventory export and data-folder backup helpers.

#include "core/InventoryTransfer.h"

#include <fstream>
#include <system_error>

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

bool backupInventatoryData(const filesystem::path& sourceDirectory,
                           const filesystem::path& destinationDirectory, string& error) {
  error_code filesystemError;
  if (!filesystem::is_directory(sourceDirectory, filesystemError)) {
    error = "Inventatory data folder does not exist: " + sourceDirectory.string();
    return false;
  }
  if (!filesystem::create_directories(destinationDirectory, filesystemError) && filesystemError) {
    error = "Unable to create backup folder: " + filesystemError.message();
    return false;
  }

  for (const auto& entry : filesystem::directory_iterator(sourceDirectory, filesystemError)) {
    if (filesystemError) break;
    if (!entry.is_regular_file(filesystemError)) {
      if (filesystemError) break;
      continue;
    }
    const auto target = destinationDirectory / entry.path().filename();
    filesystem::copy_file(entry.path(), target, filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) break;
  }
  if (filesystemError) {
    error = "Unable to copy the Inventatory data: " + filesystemError.message();
    return false;
  }
  return true;
}

}  // namespace inventatory
