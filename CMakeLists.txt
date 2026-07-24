cmake_minimum_required(VERSION 3.20)

project(InventatorySoftware VERSION 0.1.0 LANGUAGES C CXX)

set(INVENTATORY_VERSION "${PROJECT_VERSION}" CACHE STRING "Inventatory application version")

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Private-beta artifacts must run on a clean Windows PC without a separate
# Visual C++ runtime installation.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
  # Visual Studio projects may compile source files concurrently even when the
  # outer CMake build is single-node. /FS serializes shared PDB writes.
  add_compile_options(/W4 /permissive- /FS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
  # Keep the project buildable on a clean Windows developer machine without
  # requiring a globally installed SQLite SDK. This is SQLite's official public
  # domain amalgamation, pinned to an immutable release and its published hash.
  FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip
    URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
  )
  FetchContent_GetProperties(sqlite_amalgamation)
  if(NOT sqlite_amalgamation_POPULATED)
    FetchContent_Populate(sqlite_amalgamation)
  endif()
  file(GLOB sqlite_amalgamation_source
    "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c"
    "${sqlite_amalgamation_SOURCE_DIR}/*/sqlite3.c"
  )
  if(NOT sqlite_amalgamation_source)
    message(FATAL_ERROR "SQLite amalgamation download did not contain sqlite3.c")
  endif()
  add_library(inventatory_sqlite3 STATIC ${sqlite_amalgamation_source})
  get_filename_component(sqlite_amalgamation_include "${sqlite_amalgamation_source}" DIRECTORY)
  target_include_directories(inventatory_sqlite3 PUBLIC "${sqlite_amalgamation_include}")
  add_library(SQLite::SQLite3 ALIAS inventatory_sqlite3)
endif()

# miniz provides the small, permissively licensed ZIP reader used by the
# native XLSX importer.  It is MIT licensed and does not require Office,
# LibreOffice, or COM at runtime.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.1.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz)

find_package(ftxui CONFIG QUIET)
if(NOT ftxui_FOUND)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    # FTXUI v6.1.9. Pinning the commit makes source builds reproducible even
    # if its upstream tag is later moved.
    GIT_TAG 5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6
  )
FetchContent_MakeAvailable(ftxui)
endif()

add_executable(inventatory
  src/main.cpp
  src/app.cpp
  src/app/AppBootstrap.cpp
  src/app/AppActions.cpp
  src/app/AppSettings.cpp
  src/platform/StartupRegistration.cpp
  src/core/Inventory.cpp
  src/core/InventoryHelpers.cpp
  src/core/PartDescriptor.cpp
  src/core/InventoryQuery.cpp
  src/core/RackAllocation.cpp
  src/core/InventorySerialization.cpp
  src/core/InventoryHistory.cpp
  src/core/InventoryStorage.cpp
  src/core/InventoryScan.cpp
  src/core/InventorySqlite.cpp
  src/core/CatalogueDatabase.cpp
  src/core/InventatoryScanProtocol.cpp
  src/core/DeviceSyncStore.cpp
  src/import/BomCsvImport.cpp
  src/import/XlsxWorkbookReader.cpp
  src/label_printer/LabelPrinter.cpp
  src/platform/HttpServer.cpp
  src/platform/MdnsService.cpp
  src/platform/Console.cpp
  src/platform/BackgroundController.cpp
  src/platform/CredentialStore.cpp
  src/platform/BleProvisioningService.cpp
  src/platform/UpdateService.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/ActionRegistry.cpp
  src/ui/pages/DashboardPage.cpp
  src/ui/pages/StockPage.cpp
  src/ui/pages/RackManagementPage.cpp
  src/ui/pages/PrinterSetupPage.cpp
  src/ui/pages/InventatoryScanSetupPage.cpp
  src/ui/pages/OnboardingPage.cpp
  src/ui/pages/ImportCsvPage.cpp
  src/ui/pages/SettingsPage.cpp
  src/ui/pages/CataloguePage.cpp
)

target_include_directories(inventatory PRIVATE src)
target_compile_definitions(inventatory PRIVATE
  Inventatory_VERSION_STRING="${INVENTATORY_VERSION}"
  # GitHub's current MSVC toolset still needs WinRT's legacy coroutine header.
  _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
)

if(WIN32)
  target_link_libraries(inventatory PRIVATE ws2_32 shell32 winhttp comdlg32 winspool bcrypt dnsapi advapi32 runtimeobject windowsapp)
endif()

target_link_libraries(inventatory PRIVATE ftxui::screen ftxui::dom ftxui::component SQLite::SQLite3 miniz)

add_executable(inventatory_tests
  tests/inventatory_tests.cpp
  src/app/AppSettings.cpp
  src/platform/StartupRegistration.cpp
  src/platform/UpdateService.cpp
  src/import/BomCsvImport.cpp
  src/import/XlsxWorkbookReader.cpp
  src/ui/shared/AppUiShared.cpp
  src/core/Inventory.cpp
  src/core/InventoryHelpers.cpp
  src/core/PartDescriptor.cpp
  src/core/InventoryQuery.cpp
  src/core/RackAllocation.cpp
  src/core/InventorySerialization.cpp
  src/core/InventoryHistory.cpp
  src/core/InventoryStorage.cpp
  src/core/InventoryScan.cpp
  src/core/InventorySqlite.cpp
  src/core/CatalogueDatabase.cpp
  src/core/InventatoryScanProtocol.cpp
  src/core/DeviceSyncStore.cpp
  src/label_printer/LabelPrinter.cpp
)

target_include_directories(inventatory_tests PRIVATE src)

if(WIN32)
  target_link_libraries(inventatory_tests PRIVATE winspool bcrypt advapi32 winhttp)
endif()

target_link_libraries(inventatory_tests PRIVATE ftxui::screen ftxui::dom ftxui::component SQLite::SQLite3 miniz)

if(BUILD_TESTING)
  add_test(NAME inventatory_core COMMAND inventatory_tests)
endif()
