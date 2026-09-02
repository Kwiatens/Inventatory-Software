// Inventatory - Windows Firewall hardening for the Scan R1 bridge.

#pragma once

#include <cstdint>
#include <string>

namespace inventatory {

// Keeps one app-owned private-network rule aligned with the active bridge.
// Failure is non-fatal to the bridge; the returned message is intended for a
// persistent actionable warning in the terminal UI.
bool synchronizeScanFirewallRule(std::uint16_t port, const std::string& executablePath,
                                 std::string& warning);
bool removeScanFirewallRule(std::string& warning);

}  // namespace inventatory
