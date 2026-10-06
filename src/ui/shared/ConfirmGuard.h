// Inventatory - two-step confirmation for actions that discard in-memory work.
//
// The first request only arms the guard (the caller tells the user what will be
// lost); repeating the request inside the window confirms it. This matches the
// existing timed confirmations (token regeneration, clearing a device) and
// works for keyboard and mouse alike.

#pragma once

#include <ctime>

namespace inventatory::confirm_guard {

// True when the action may proceed. With `needsConfirmation` false the guard is
// disarmed and the action proceeds at once.
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
