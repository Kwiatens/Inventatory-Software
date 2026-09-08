// Inventatory - Hardware Inventory Management System
// Alert-led dashboard rendering and keyboard handling.

#include "App.h"

#include <algorithm>
#include <cctype>

namespace inventatory {

using namespace std;

void App::handleDashboardKey(const KeyEvent& key) {
  if (key.type == KeyType::Left) {
    selectDashboardRow(DashboardList::Warnings, dashboardWarningSelection_);
    return;
  }
  if (key.type == KeyType::Right) {
    selectDashboardRow(DashboardList::Commits, dashboardActivitySelection_);
    return;
  }
  if (key.type == KeyType::Up) {
    moveDashboardSelection(dashboardList_, -1);
    return;
  }
  if (key.type == KeyType::Down) {
    moveDashboardSelection(dashboardList_, 1);
    return;
  }
  if (key.type == KeyType::PageUp) {
    moveDashboardSelection(dashboardList_, -10);
    return;
  }
  if (key.type == KeyType::PageDown) {
    moveDashboardSelection(dashboardList_, 10);
    return;
  }
  if (key.type == KeyType::Home) {
    jumpDashboardSelection(dashboardList_, false);
    return;
  }
  if (key.type == KeyType::End) {
    jumpDashboardSelection(dashboardList_, true);
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = tolower(static_cast<unsigned char>(key.ch));
    if (ch == 'j') {
      moveDashboardSelection(dashboardList_, 1);
      return;
    }
    if (ch == 'k') {
      moveDashboardSelection(dashboardList_, -1);
      return;
    }
    switch (ch) {
      case '1':
      case '\t':
        changePage(Page::Stock);
        break;
      case 'n':
        beginEditCurrentItem(true);
        break;
      case '4':
      case 'r':
      case 'd':
        {
          InventoryStore loadedStore;
          vector<InventoryHistoryPoint> loadedHistory;
          if (!loadedStore.load(inventoryPath_)) {
            persistenceError_ = "Unable to reload the inventory database; the in-memory data was kept.";
            setMessage(persistenceError_, 6);
            break;
          }
          if (!loadInventoryHistory(inventoryPath_, loadedHistory)) {
            inventoryRecoveryRequired_ = true;
            inventoryRecoveryDetail_ = "Unable to reload inventory history: " + inventoryPath_.string();
            persistenceError_ = inventoryRecoveryDetail_ + ". The original history was preserved.";
            setMessage(persistenceError_, 6);
            break;
          }
          if (loadedHistory.empty()) {
            appendInventoryHistory(loadedHistory,
                                   makeInventoryHistoryPoint(loadedStore.items(), settings_.lowStockThreshold));
          }
          if (!saveInventoryHistory(inventoryPath_, loadedHistory)) {
            inventoryRecoveryRequired_ = true;
            inventoryRecoveryDetail_ = "Unable to save the reloaded inventory history: " + inventoryPath_.string();
            persistenceError_ = inventoryRecoveryDetail_ + ". The original history was preserved.";
            setMessage(persistenceError_, 6);
            break;
          }
          refreshInventoryCommits();
          if (inventoryRecoveryRequired_) break;
          store_ = move(loadedStore);
          inventoryHistory_ = move(loadedHistory);
          persistedStore_ = store_;
          persistedStoreValid_ = true;
          refreshInventoryMovements();
        }
        setMessage("Inventory reloaded from the database", 2);
        break;
      case '5':
      case 'i':
        beginCsvImport();
        break;
      case 'm':
        openRackManagement();
        break;
      case 'u':
        openInventatoryScanSetup();
        break;
      case 'f':
        changePage(Page::Stock);
        startSearch();
        break;
      case 'l':
        openPrinterSetup();
        break;
      case '/':
        startSearch();
        break;
      case 'q':
        requestUserExit();
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Enter && dashboardList_ == DashboardList::Commits && !inventoryCommits_.empty()) {
    historySelection_ = min(dashboardActivitySelection_, inventoryCommits_.size() - 1);
    openSelectedHistoryCommit();
    return;
  }
  if (key.type == KeyType::Enter || key.type == KeyType::Tab) changePage(Page::Stock);
}
}  // namespace inventatory
