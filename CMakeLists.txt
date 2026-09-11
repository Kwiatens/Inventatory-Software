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
  # Keep the parser fix for consecutive/truncated Escape sequences. Pinning
  # the commit makes source builds reproducible even if its upstream tag moves.
  GIT_TAG 182ef70cd4dc1109d06e66361a9da86445928578
)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "Do not package FTXUI separately" FORCE)
FetchContent_MakeAvailable(ftxui)

add_executable(inventatory
  src/main.cpp
  src/app.cpp
  src/app/shell/AppInput.cpp
  src/app/shell/AppInputModes.cpp
  src/app/shell/AppRuntime.cpp
  src/app/shell/AppShell.cpp
  src/app/shell/AppShellRender.cpp
  src/app/shell/AppBootstrap.cpp
  src/app/persistence/AppPersistence.cpp
  src/app/persistence/AppHistoryPersistence.cpp
  src/app/persistence/AppBackupRestore.cpp
  src/app/workspace/AppWorkspace.cpp
  src/app/inventory/AppInventoryEdit.cpp
  src/app/inventory/AppInventorySelection.cpp
  src/app/racks/AppRackActions.cpp
  src/app/racks/AppRackPrintingActions.cpp
  src/app/racks/AppRackPlacementActions.cpp
  src/app/scanner/AppDeviceActions.cpp
  src/app/labels/AppQuickLabelActions.cpp
  src/app/scanner/AppScanEnrichment.cpp
  src/app/import/AppImportActions.cpp
  src/app/bom/AppBomEnrichment.cpp
  src/app/bom/AppBomActions.cpp
  src/app/bom/AppBomProjectActions.cpp
  src/app/bom/AppBomBuildActions.cpp
  src/app/settings/AppSettings.cpp
  src/app/settings/AppSettingsAppearance.cpp
  src/platform/system/StartupRegistration.cpp
  src/core/inventory/Inventory.cpp
  src/core/inventory/InventoryHelpers.cpp
  src/core/inventory/InventoryIdentifiers.cpp
  src/core/parts/PartDescriptor.cpp
  src/core/parts/PartDescriptorContext.cpp
  src/core/parts/PartDescriptorRules.cpp
  src/core/query/InventoryQuery.cpp
  src/core/query/InventoryQueryMatching.cpp
  src/core/parts/PhysicalValue.cpp
  src/core/racks/RackAllocation.cpp
  src/core/inventory/InventorySerialization.cpp
  src/core/history/InventoryHistory.cpp
  src/core/history/InventoryVersion.cpp
  src/core/history/InventoryVersionHistory.cpp
  src/core/history/InventoryVersionDiff.cpp
  src/core/inventory/InventoryMovements.cpp
  src/core/storage/AtomicFile.cpp
  src/core/storage/InventoryStorage.cpp
  src/core/storage/InventoryStorageSqlite.cpp
  src/core/transfer/InventoryTransfer.cpp
  src/core/transfer/InventoryBackup.cpp
  src/core/transfer/InventoryRestore.cpp
  src/core/transfer/InventoryTransferManifest.cpp
  src/core/transfer/InventoryTransferOps.cpp
  src/core/transfer/InventoryTransferRecovery.cpp
  src/core/transfer/InventoryTransferValidation.cpp
  src/core/inventory/InventoryScan.cpp
  src/core/storage/InventorySqlite.cpp
  src/core/storage/InventorySqliteSchema.cpp
  src/core/scanner/InventatoryScanProtocol.cpp
  src/core/scanner/InventatoryScanProtocolConfig.cpp
  src/core/scanner/InventatoryScanProtocolJson.cpp
  src/core/scanner/InventatoryScanProtocolJsonParser.cpp
  src/core/scanner/InventatoryScanProtocolSecurity.cpp
  src/core/scanner/DeviceSyncStore.cpp
  src/core/bom/BomMatch.cpp
  src/core/bom/BomMatchHelpers.cpp
  src/core/bom/BomProjectStore.cpp
  src/import/csv/CsvReader.cpp
  src/import/csv/CsvFormat.cpp
  src/import/kicad/KicadBom.cpp
  src/import/digikey/DigiKeyCsvImport.cpp
  src/label_printer/core/LabelPrinter.cpp
  src/label_printer/core/LabelPrinterContext.cpp
  src/label_printer/core/LabelPrinterDetails.cpp
  src/label_printer/platform/LabelPrinterPlatform.cpp
  src/label_printer/platform/LabelPrinterWindows.cpp
  src/label_printer/platform/LabelPrinterRack.cpp
  src/label_printer/layout/LabelPrinterText.cpp
  src/label_printer/layout/LabelPrinterTextLayout.cpp
  src/platform/scanner/HttpServer.cpp
  src/platform/scanner/HttpServerConnection.cpp
  src/platform/scanner/HttpServerProtocol.cpp
  src/platform/scanner/HttpServerLifecycle.cpp
  src/platform/scanner/MdnsService.cpp
  src/platform/digikey/DigiKeyApi.cpp
  src/platform/digikey/DigiKeyJson.cpp
  src/platform/digikey/DigiKeyJsonParser.cpp
  src/platform/digikey/DigiKeyProduct.cpp
  src/platform/digikey/DigiKeyProductDetails.cpp
  src/platform/digikey/DigiKeyTransport.cpp
  src/platform/system/Console.cpp
  src/platform/system/Environment.cpp
  src/platform/system/BackgroundController.cpp
  src/platform/security/CredentialStore.cpp
  src/platform/scanner/BleProvisioningService.cpp
  src/platform/system/UpdateService.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/shared/AppUiItemDetails.cpp
  src/ui/ActionRegistry.cpp
  src/ui/pages/dashboard/DashboardPage.cpp
  src/ui/pages/dashboard/DashboardPageRender.cpp
  src/ui/pages/dashboard/DashboardPageComponents.cpp
  src/ui/pages/stock/StockPage.cpp
  src/ui/pages/stock/StockPageList.cpp
  src/ui/pages/stock/StockPageDetail.cpp
  src/ui/pages/stock/StockPageInput.cpp
  src/ui/pages/stock/StockPageStocktake.cpp
  src/ui/pages/racks/RackManagementPage.cpp
  src/ui/pages/printer/PrinterSetupPage.cpp
  src/ui/pages/scanner/InventatoryScanSetupPage.cpp
  src/ui/pages/scanner/InventatoryScanSetupPageRender.cpp
  src/ui/pages/digikey/DigiKeySetupPage.cpp
  src/ui/pages/onboarding/OnboardingPage.cpp
  src/ui/pages/import/ImportCsvPage.cpp
  src/ui/pages/bom/BomProjectPage.cpp
  src/ui/pages/bom/BomProjectPageRender.cpp
  src/ui/pages/history/HistoryPage.cpp
  src/ui/pages/history/HistoryPageRender.cpp
  src/ui/pages/settings/SettingsPage.cpp
  src/ui/pages/settings/SettingsPageAppearance.cpp
  src/ui/pages/settings/SettingsPageInput.cpp
  src/ui/pages/settings/SettingsPageState.cpp
  src/ui/pages/settings/SettingsPageSave.cpp
)

if(WIN32)
  add_executable(inventatory_background WIN32
    src/platform/system/BackgroundLauncher.cpp
    src/platform/system/BackgroundLauncher.rc
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
  src/app/shell/AppBootstrap.cpp
  src/app/settings/AppSettings.cpp
  src/app/settings/AppSettingsAppearance.cpp
  src/platform/system/StartupRegistration.cpp
  src/platform/system/UpdateService.cpp
  src/platform/scanner/HttpServer.cpp
  src/platform/scanner/HttpServerConnection.cpp
  src/platform/scanner/HttpServerProtocol.cpp
  src/platform/scanner/HttpServerLifecycle.cpp
  src/platform/system/Console.cpp
  src/platform/system/Environment.cpp
  src/platform/security/CredentialStore.cpp
  src/platform/digikey/DigiKeyApi.cpp
  src/platform/digikey/DigiKeyJson.cpp
  src/platform/digikey/DigiKeyJsonParser.cpp
  src/platform/digikey/DigiKeyProduct.cpp
  src/platform/digikey/DigiKeyProductDetails.cpp
  src/platform/digikey/DigiKeyTransport.cpp
  src/import/csv/CsvReader.cpp
  src/import/csv/CsvFormat.cpp
  src/import/kicad/KicadBom.cpp
  src/import/digikey/DigiKeyCsvImport.cpp
  src/core/bom/BomMatch.cpp
  src/core/bom/BomMatchHelpers.cpp
  src/core/bom/BomProjectStore.cpp
  src/ui/shared/AppUiShared.cpp
  src/ui/shared/AppUiItemDetails.cpp
  src/core/inventory/Inventory.cpp
  src/core/inventory/InventoryHelpers.cpp
  src/core/inventory/InventoryIdentifiers.cpp
  src/core/parts/PartDescriptor.cpp
  src/core/parts/PartDescriptorContext.cpp
  src/core/parts/PartDescriptorRules.cpp
  src/core/query/InventoryQuery.cpp
  src/core/query/InventoryQueryMatching.cpp
  src/core/parts/PhysicalValue.cpp
  src/core/racks/RackAllocation.cpp
  src/core/inventory/InventorySerialization.cpp
  src/core/history/InventoryHistory.cpp
  src/core/history/InventoryVersion.cpp
  src/core/history/InventoryVersionHistory.cpp
  src/core/history/InventoryVersionDiff.cpp
  src/core/inventory/InventoryMovements.cpp
  src/core/storage/AtomicFile.cpp
  src/core/storage/InventoryStorage.cpp
  src/core/storage/InventoryStorageSqlite.cpp
  src/core/transfer/InventoryTransfer.cpp
  src/core/transfer/InventoryBackup.cpp
  src/core/transfer/InventoryRestore.cpp
  src/core/transfer/InventoryTransferManifest.cpp
  src/core/transfer/InventoryTransferOps.cpp
  src/core/transfer/InventoryTransferRecovery.cpp
  src/core/transfer/InventoryTransferValidation.cpp
  src/core/inventory/InventoryScan.cpp
  src/core/storage/InventorySqlite.cpp
  src/core/storage/InventorySqliteSchema.cpp
  src/core/scanner/InventatoryScanProtocol.cpp
  src/core/scanner/InventatoryScanProtocolConfig.cpp
  src/core/scanner/InventatoryScanProtocolJson.cpp
  src/core/scanner/InventatoryScanProtocolJsonParser.cpp
  src/core/scanner/InventatoryScanProtocolSecurity.cpp
  src/core/scanner/DeviceSyncStore.cpp
  src/label_printer/core/LabelPrinter.cpp
  src/label_printer/core/LabelPrinterContext.cpp
  src/label_printer/core/LabelPrinterDetails.cpp
  src/label_printer/platform/LabelPrinterPlatform.cpp
  src/label_printer/platform/LabelPrinterWindows.cpp
  src/label_printer/platform/LabelPrinterRack.cpp
  src/label_printer/layout/LabelPrinterText.cpp
  src/label_printer/layout/LabelPrinterTextLayout.cpp
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

add_executable(inventatory_input_tests
  tests/terminal_input_parser_tests.cpp
)

target_include_directories(inventatory_input_tests PRIVATE ${ftxui_SOURCE_DIR}/src)
target_link_libraries(inventatory_input_tests PRIVATE ftxui::screen ftxui::dom ftxui::component)

if(BUILD_TESTING)
  add_test(NAME inventatory_core COMMAND inventatory_tests)
  add_test(NAME inventatory_input COMMAND inventatory_input_tests)
endif()
