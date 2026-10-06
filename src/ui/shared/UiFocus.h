// Inventatory - keyboard focus tracking for clickable terminal targets.
//
// Focus is identified by the stable target id rather than by the position in
// the per-frame target list, which changes whenever rows are filtered, added
// or removed. Only Tab / Shift+Tab move it; every other key and every mouse
// click drops it, so Enter keeps meaning "run the page's primary action" unless
// the user has visibly tabbed to a control.

#pragma once

#include <string>
#include <vector>

namespace inventatory::ui_focus {

struct Candidate {
  std::string id;
  bool enabled = true;
  bool focusable = true;
};

inline bool canFocus(const Candidate& candidate) {
  return candidate.enabled && candidate.focusable && !candidate.id.empty();
}

// Index of the first target with this id, or -1.
inline int indexOf(const std::vector<Candidate>& targets, const std::string& id) {
  if (id.empty()) return -1;
  for (size_t index = 0; index < targets.size(); ++index) {
    if (targets[index].id == id) return static_cast<int>(index);
  }
  return -1;
}

// The id Tab (delta > 0) or Shift+Tab (delta < 0) moves to, wrapping around.
// With no current focus, Tab lands on the first and Shift+Tab on the last
// focusable target. Returns an empty string when nothing can take focus.
inline std::string next(const std::vector<Candidate>& targets, const std::string& current, int delta) {
  if (targets.empty() || delta == 0) return {};
  const int count = static_cast<int>(targets.size());
  const int step = delta > 0 ? 1 : -1;
  int position = indexOf(targets, current);
  if (position < 0) position = step > 0 ? -1 : count;
  for (int visited = 0; visited < count; ++visited) {
    position = ((position + step) % count + count) % count;
    if (canFocus(targets[static_cast<size_t>(position)])) return targets[static_cast<size_t>(position)].id;
  }
  return {};
}

// True when the focus id still names an enabled, focusable target.
inline bool isActivatable(const std::vector<Candidate>& targets, const std::string& focused) {
  const int index = indexOf(targets, focused);
  return index >= 0 && canFocus(targets[static_cast<size_t>(index)]);
}

}  // namespace inventatory::ui_focus
