cmake_minimum_required(VERSION 3.20)

project(InventatorySoftware VERSION 0.1.0 LANGUAGES C CXX)

option(INVENTATORY_PARALLEL_MSVC "Enable MSVC parallel compilation" ON)

if(MSVC)
    if(INVENTATORY_PARALLEL_MSVC)
        add_compile_options(/MP)
    endif()
endif()

set(INVENTATORY_VERSION "${PROJECT_VERSION}" CACHE STRING "Inventatory application version")
set(INVENTATORY_RELEASE_REPOSITORY "" CACHE STRING "GitHub owner/repository for application updates")
set(INVENTATORY_SCAN_FIRMWARE_REPOSITORY "" CACHE STRING "GitHub owner/repository for Scan R1 firmware updates")

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release artifacts must run on a clean Windows PC without a separate
# Visual C++ runtime installation.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

# SQLite is compiled into the executable so the release never depends on an
# ambient sqlite3.dll. The official source archive and its published SHA3-256
# pin the exact amalgamation used by every build.
FetchContent_Declare(sqlite3_amalgamation
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3_amalgamation)

add_library(sqlite3_amalgamation STATIC
  ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c
)
target_include_directories(sqlite3_amalgamation PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  # FTXUI v6.1.9. Pinning the commit makes source builds reproducible even
  # if its upstream tag is later moved.
  GIT_TAG 5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6
)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "Do not package FTXUI separately" FORCE)
FetchContent_MakeAvailable(ftxui)

add_executable(inventatory
  src/main.cpp
  src/app.cpp
  src/app/AppInput.cpp
  src/app/AppInputModes.cpp
  src/app/AppRuntime.cpp
  src/app/AppShell.cpp
  src/app/AppShellRender.cpp
  src/app/AppBootstrap.cpp
  src/app/AppPersistence.cpp
  src/app/AppHistoryPersistence.cpp
  src/app/AppBackupRestore.cpp
  src/app/AppWorkspace.cpp
  src/app/AppInventoryEdit.cpp
  src/app/AppInventorySelection.cpp
  src/app/AppRackActions.cpp
  src/app/AppDeviceActions.cpp
  src/app/AppQuickLabelActions.cpp
  src/app/AppScanEnrichment.cpp
  src/app/AppImportActions.cpp
  src/app/AppBomEnrichment.cpp
  src/app/AppBomActions.cpp
  src/app/AppBomProjectActions.cpp
  src/app/AppSettings.cpp
  src/platform/StartupRegistration.cpp
  src/core/Inventory.cpp
  src/core/InventoryHelpers.cpp
  src/core/PartDescriptor.cpp
  src/core/PartDescriptorContext.cpp
  src/core/PartDescriptorRules.cpp
  src/core/InventoryQuery.cpp
  src/core/PhysicalValue.cpp
  src/core/RackAllocation.cpp
  src/core/InventorySerialization.cpp
  src/core/InventoryHistory.cpp
  src/core/InventoryVersion.cpp
  src/core/InventoryVersionDiff.cpp
  src/core/InventoryMovements.cpp
  src/core/AtomicFile.cpp
  src/core/InventoryStorage.cpp
  src/core/InventoryStorageSqlite.cpp
  src/core/InventoryTransfer.cpp
  src/core/InventoryTransferManifest.cpp
  src/core/InventoryTransferOps.cpp
  src/core/InventoryTransferRecovery.cpp
  src/core/InventoryTransferValidation.cpp
  src/core/InventoryScan.cpp
  src/core/InventorySqlite.cpp
  src/core/InventorySqliteSchema.cpp
  src/core/InventatoryScanProtocol.cpp
  src/core/InventatoryScanProtocolConfig.cpp
  src/core/InventatoryScanProtocolJson.cpp
  src/core/InventatoryScanProtocolSecurity.cpp
  src/core/DeviceSyncStore.cpp
  src/core/BomMatch.cpp
  src/core/BomProjectStore.cpp
  src/import/CsvReader.cpp
  src/import/CsvFormat.cpp
  src/import/KicadBom.cpp
  src/import/DigiKeyCsvImport.cpp
  src/label_printer/LabelPrinter.cpp
  src/label_printer/LabelPrinterContext.cpp
  src/label_printer/LabelPrinterDetails.cpp
  src/label_printer/LabelPrinterPlatform.cpp
  src/label_printer/LabelPrinterRack.cpp
  src/label_printer/LabelPrinterText.cpp
  src/platform/HttpServer.cpp
  src/platform/HttpServerProtocol.cpp
  src/platform/HttpServerLifecycle.cpp
  src/platform/MdnsService.cpp
  src/platform/DigiKeyApi.cpp
  src/platform/DigiKeyJson.cpp
  src/platform/DigiKeyJsonParser.cpp
  src/platform/DigiKeyProduct.cpp
  src/platform/DigiKeyTransport.cpp
  src/platform/Console.cpp
  src/platform/Environment.cpp
  src/platform/BackgroundController.cpp
  src/platform/CredentialStore.cpp
  src/platform/BleProvisioningService.cpp
  src/platform/UpdateService.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/shared/AppUiItemDetails.cpp
  src/ui/ActionRegistry.cpp
  src/ui/pages/DashboardPage.cpp
  src/ui/pages/DashboardPageRender.cpp
  src/ui/pages/StockPage.cpp
  src/ui/pages/StockPageList.cpp
  src/ui/pages/StockPageDetail.cpp
  src/ui/pages/StockPageInput.cpp
  src/ui/pages/StockPageStocktake.cpp
  src/ui/pages/RackManagementPage.cpp
  src/ui/pages/PrinterSetupPage.cpp
  src/ui/pages/InventatoryScanSetupPage.cpp
  src/ui/pages/DigiKeySetupPage.cpp
  src/ui/pages/OnboardingPage.cpp
  src/ui/pages/ImportCsvPage.cpp
  src/ui/pages/BomProjectPage.cpp
  src/ui/pages/BomProjectPageRender.cpp
  src/ui/pages/HistoryPage.cpp
  src/ui/pages/HistoryPageRender.cpp
  src/ui/pages/SettingsPage.cpp
  src/ui/pages/SettingsPageInput.cpp
  src/ui/pages/SettingsPageState.cpp
  src/ui/pages/SettingsPageSave.cpp
)

if(WIN32)
  add_executable(inventatory_background WIN32
    src/platform/BackgroundLauncher.cpp
    src/platform/BackgroundLauncher.rc
  )
  set_target_properties(inventatory_background PROPERTIES OUTPUT_NAME "inventatory-background")
endif()

target_include_directories(inventatory PRIVATE src)
target_compile_definitions(inventatory PRIVATE
  Inventatory_VERSION_STRING="${INVENTATORY_VERSION}"
  Inventatory_RELEASE_REPOSITORY="${INVENTATORY_RELEASE_REPOSITORY}"
  Inventatory_SCAN_FIRMWARE_REPOSITORY="${INVENTATORY_SCAN_FIRMWARE_REPOSITORY}"
  # GitHub's current MSVC toolset still needs WinRT's legacy coroutine header.
  _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
)

if(WIN32)
  target_link_libraries(inventatory PRIVATE ws2_32 shell32 winhttp comdlg32 winspool bcrypt dnsapi advapi32 runtimeobject windowsapp)
endif()

target_link_libraries(inventatory PRIVATE sqlite3_amalgamation ftxui::screen ftxui::dom ftxui::component)

add_executable(inventatory_tests
  tests/inventatory_tests.cpp
  src/app/AppBootstrap.cpp
  src/app/AppSettings.cpp
  src/platform/StartupRegistration.cpp
  src/platform/UpdateService.cpp
  src/platform/HttpServer.cpp
  src/platform/HttpServerProtocol.cpp
  src/platform/HttpServerLifecycle.cpp
  src/platform/Console.cpp
  src/platform/Environment.cpp
  src/platform/CredentialStore.cpp
  src/platform/DigiKeyApi.cpp
  src/platform/DigiKeyJson.cpp
  src/platform/DigiKeyJsonParser.cpp
  src/platform/DigiKeyProduct.cpp
  src/platform/DigiKeyTransport.cpp
  src/import/CsvReader.cpp
  src/import/CsvFormat.cpp
  src/import/KicadBom.cpp
  src/import/DigiKeyCsvImport.cpp
  src/core/BomMatch.cpp
  src/core/BomProjectStore.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/shared/AppUiItemDetails.cpp
  src/core/Inventory.cpp
  src/core/InventoryHelpers.cpp
  src/core/PartDescriptor.cpp
  src/core/PartDescriptorContext.cpp
  src/core/PartDescriptorRules.cpp
  src/core/InventoryQuery.cpp
  src/core/PhysicalValue.cpp
  src/core/RackAllocation.cpp
  src/core/InventorySerialization.cpp
  src/core/InventoryHistory.cpp
  src/core/InventoryVersion.cpp
  src/core/InventoryVersionDiff.cpp
  src/core/InventoryMovements.cpp
  src/core/AtomicFile.cpp
  src/core/InventoryStorage.cpp
  src/core/InventoryStorageSqlite.cpp
  src/core/InventoryTransfer.cpp
  src/core/InventoryTransferManifest.cpp
  src/core/InventoryTransferOps.cpp
  src/core/InventoryTransferRecovery.cpp
  src/core/InventoryTransferValidation.cpp
  src/core/InventoryScan.cpp
  src/core/InventorySqlite.cpp
  src/core/InventorySqliteSchema.cpp
  src/core/InventatoryScanProtocol.cpp
  src/core/InventatoryScanProtocolConfig.cpp
  src/core/InventatoryScanProtocolJson.cpp
  src/core/InventatoryScanProtocolSecurity.cpp
  src/core/DeviceSyncStore.cpp
  src/label_printer/LabelPrinter.cpp
  src/label_printer/LabelPrinterContext.cpp
  src/label_printer/LabelPrinterDetails.cpp
  src/label_printer/LabelPrinterPlatform.cpp
  src/label_printer/LabelPrinterRack.cpp
  src/label_printer/LabelPrinterText.cpp
)

target_include_directories(inventatory_tests PRIVATE src)
target_compile_definitions(inventatory_tests PRIVATE
  Inventatory_RELEASE_REPOSITORY="${INVENTATORY_RELEASE_REPOSITORY}"
  Inventatory_SCAN_FIRMWARE_REPOSITORY="${INVENTATORY_SCAN_FIRMWARE_REPOSITORY}"
)

if(WIN32)
  target_link_libraries(inventatory_tests PRIVATE ws2_32 winspool winhttp bcrypt advapi32)
endif()

target_link_libraries(inventatory_tests PRIVATE sqlite3_amalgamation ftxui::screen ftxui::dom ftxui::component)

if(BUILD_TESTING)
  add_test(NAME inventatory_core COMMAND inventatory_tests)
endif()
