// Inventatory - Hardware Inventory Management System
// Durable storage for pinned BOM projects.

#pragma once

#include "core/Inventory.h"

#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::map;
using std::string;
using std::time_t;
using std::vector;

#ifdef _WIN32
struct SqliteConnection;
#endif

// A pinned project keeps the BOM text rather than a file reference, so the
// analysis can be rebuilt against current stock without the original file.
struct BomProject {
  string id;
  string name;
  string sourcePath;
  int boards = 1;
  time_t createdAt = 0;
  time_t lastOpened = 0;
  time_t lastBuilt = 0;
  string bomText;
  map<string, string> overrides;   // bomLineKey -> chosen item id
  map<string, string> enrichment;  // bomLineKey -> suggested DigiKey part
};

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects);
bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects);
#ifdef _WIN32
// Validate the persisted BOM rows without migrating or mutating the database.
// Backup validation uses this read-only seam before a bundle can be activated.
bool validateBomProjects(SqliteConnection& connection, string* error = nullptr);
#endif

// Shared with the tests: escaped round trip for the two string maps.
string serializeBomMap(const map<string, string>& values);
map<string, string> deserializeBomMap(const string& value);

}  // namespace inventatory
