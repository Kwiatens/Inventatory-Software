// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Two-step confirmation for actions that discard in-memory work: the first request
// arms the guard, and a repeat inside the window confirms.

#pragma once

#include <ctime>

namespace inventatory::confirm_guard {

// True when the action may proceed. The first call only arms the guard.
inline bool confirmed(std::time_t now, std::time_t& armedUntil, int windowSeconds, bool needsConfirmation) {
  if (!needsConfirmation) {
    armedUntil = 0;
    return true;
  }
  if (armedUntil != 0 && now <= armedUntil) {
    armedUntil = 0;
    return true;
  }
  armedUntil = now + windowSeconds;
  return false;
}

}  // namespace inventatory::confirm_guard
