// Inventatory - Hardware Inventory Management System
// KiCad BOM project input: pinned list, unified analysis, build walkthrough.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;



void App::handleBomProjectKey(const KeyEvent& key) {
  if (bomDeductPrompt_) {
    if (key.type == KeyType::Enter) {
      finishBomBuild(true);
      return;
    }
    if (key.type == KeyType::Escape) {
      if (bomBuildReady(bomAnalysis_)) {
        finishBomBuild(false);
      } else {
        bomDeductPrompt_ = false;
        bomView_ = BomView::Split;
        dirty_ = true;
      }
    }
    return;  // y / n are registered actions and dispatch ahead of this handler
  }

  if (bomView_ == BomView::Build) {
    if (key.type == KeyType::Enter) {
      advanceBomBuild(1);
    } else if (key.type == KeyType::Backspace || key.type == KeyType::Left) {
      advanceBomBuild(-1);
    } else if (key.type == KeyType::Right) {
      advanceBomBuild(1);
    } else if (key.type == KeyType::Escape) {
      bomView_ = BomView::Split;
      bomBuildStep_ = 0;
      dirty_ = true;
    }
    return;
  }

  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const bool narrowAnalysis = activeScreen != nullptr && activeScreen->dimx() < 132;
  if (narrowAnalysis && bomInspectorOpen_) {
    if (key.type == KeyType::Escape || key.type == KeyType::Left) {
      bomInspectorOpen_ = false;
      dirty_ = true;
    } else if (key.type == KeyType::Enter && !bomAnalysis_.matches.empty() &&
               !bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)].sufficient) {
      beginBomRestock();
    }
    return;
  }

  // Everything with a visible accelerator lives in the action registry, which
  // dispatches ahead of this handler; only raw navigation is handled here.
  if (bomView_ == BomView::List || !bomAnalysisValid_) {
    if (key.type == KeyType::Character) {
      const auto ch = tolower(static_cast<unsigned char>(key.ch));
      if (ch == 'j') {
        moveBomSelection(1);
      } else if (ch == 'k') {
        moveBomSelection(-1);
      }
      return;
    }
    if (key.type == KeyType::Up) {
      moveBomSelection(-1);
    } else if (key.type == KeyType::Down) {
      moveBomSelection(1);
    } else if (key.type == KeyType::PageUp) {
      moveBomSelection(-10);
    } else if (key.type == KeyType::PageDown) {
      moveBomSelection(10);
    } else if (key.type == KeyType::Enter) {
      openSelectedBomProject();
    } else if (key.type == KeyType::Escape) {
      changePage(Page::Home);
    }
    return;
  }

  if (key.type == KeyType::Character) {
    switch (tolower(static_cast<unsigned char>(key.ch))) {
      case 'j':
        moveBomSelection(1);
        break;
      case 'k':
        moveBomSelection(-1);
        break;
      // '=' and '_' are the unshifted twins of the registered '+' and '-'.
      case '=':
        adjustBomBoards(1);
        break;
      case '_':
        adjustBomBoards(-1);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Up) {
    moveBomSelection(-1);
  } else if (key.type == KeyType::Down) {
    moveBomSelection(1);
  } else if (key.type == KeyType::PageUp) {
    moveBomSelection(-10);
  } else if (key.type == KeyType::PageDown) {
    moveBomSelection(10);
  } else if (key.type == KeyType::Home) {
    bomSplitSelection_ = 0;
    dirty_ = true;
  } else if (key.type == KeyType::End) {
    if (!bomAnalysis_.matches.empty()) {
      bomSplitSelection_ = bomAnalysis_.matches.size() - 1;
      dirty_ = true;
    }
  } else if (key.type == KeyType::Enter) {
    if (narrowAnalysis) {
      bomInspectorOpen_ = true;
      dirty_ = true;
    } else if (!bomAnalysis_.matches.empty() &&
               !bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)].sufficient) {
      beginBomRestock();
    }
  } else if (key.type == KeyType::Escape) {
    bomView_ = BomView::List;
    dirty_ = true;
  }
}

}  // namespace inventatory
