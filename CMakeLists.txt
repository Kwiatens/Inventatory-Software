cmake_minimum_required(VERSION 3.20)

project(HIMSSoftware VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

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

add_executable(hims
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
  src/core/HimsScanProtocol.cpp
  src/core/DeviceSyncStore.cpp
  src/import/DigiKeyCsvImport.cpp
  src/label_printer/LabelPrinter.cpp
  src/platform/HttpServer.cpp
  src/platform/MdnsService.cpp
  src/platform/DigiKeyApi.cpp
  src/platform/Console.cpp
  src/platform/BackgroundController.cpp
  src/platform/CredentialStore.cpp
  src/platform/BleProvisioningService.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/ActionRegistry.cpp
  src/ui/pages/DashboardPage.cpp
  src/ui/pages/StockPage.cpp
  src/ui/pages/RackManagementPage.cpp
  src/ui/pages/PrinterSetupPage.cpp
  src/ui/pages/HimsScanSetupPage.cpp
  src/ui/pages/ImportCsvPage.cpp
  src/ui/pages/SettingsPage.cpp
)

target_include_directories(hims PRIVATE src)
target_compile_definitions(hims PRIVATE HIMS_VERSION_STRING="${PROJECT_VERSION}")

if(WIN32)
  target_link_libraries(hims PRIVATE ws2_32 shell32 winhttp comdlg32 winspool bcrypt dnsapi advapi32 runtimeobject windowsapp)
endif()

target_link_libraries(hims PRIVATE ftxui::screen ftxui::dom ftxui::component)

add_executable(hims_tests
  tests/hims_tests.cpp
  src/app/AppSettings.cpp
  src/platform/StartupRegistration.cpp
  src/import/DigiKeyCsvImport.cpp
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
  src/core/HimsScanProtocol.cpp
  src/core/DeviceSyncStore.cpp
  src/label_printer/LabelPrinter.cpp
)

target_include_directories(hims_tests PRIVATE src)

if(WIN32)
  target_link_libraries(hims_tests PRIVATE winspool bcrypt advapi32)
endif()

target_link_libraries(hims_tests PRIVATE ftxui::screen ftxui::dom ftxui::component)

if(BUILD_TESTING)
  add_test(NAME hims_core COMMAND hims_tests)
endif()
