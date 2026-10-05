// Inventatory - Parsing of CUPS "lpstat -p" queue lines. Header-only so it is
// unit-testable on every platform.

#pragma once

#include <optional>
#include <string>

namespace inventatory {

struct CupsQueueLine {
  std::string name;
  std::string status;  // text after the queue name, without a leading "is "
  bool ready = true;
};

// Parses one line of `lpstat -p` output produced under LC_ALL=C, for example
//   printer Zebra is idle.  enabled since Tue 05 Oct 2026 10:00:00
//   printer Zebra now printing Zebra-12.  enabled since Tue 05 Oct 2026 10:00:00
//   printer Zebra disabled since Tue 05 Oct 2026 10:00:00 -
// Returns nullopt for any other line (reason lines, the default destination, empty lines).
inline std::optional<CupsQueueLine> parseCupsQueueLine(const std::string& line) {
  const std::string prefix = "printer ";
  if (line.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  const auto nameEnd = line.find(' ', prefix.size());
  if (nameEnd == std::string::npos || nameEnd == prefix.size()) return std::nullopt;
  CupsQueueLine parsed;
  parsed.name = line.substr(prefix.size(), nameEnd - prefix.size());
  parsed.status = line.substr(nameEnd + 1U);
  if (parsed.status.compare(0, 3, "is ") == 0) parsed.status.erase(0, 3);
  while (!parsed.status.empty() && (parsed.status.back() == '\r' || parsed.status.back() == '\n')) {
    parsed.status.pop_back();
  }
  std::string lowered = parsed.status;
  for (auto& character : lowered) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  parsed.ready = lowered.find("disabled") == std::string::npos && lowered.find("stopped") == std::string::npos;
  return parsed;
}

}  // namespace inventatory
