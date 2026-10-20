// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/component/terminal_input_parser.hpp>

// Keep asserts enabled in every build type; CMake's Release config defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
  {
    // Escape also prefixes terminal sequences, so repeated Escapes stay separate events.
    std::vector<ftxui::Event> events;
    ftxui::TerminalInputParser parser([&events](ftxui::Event event) {
      events.push_back(std::move(event));
    });
    for (const char character : {'\x1B', '\x1B', '\x1B'}) {
      parser.Add(character);
    }
    parser.Timeout(50);

    assert(events.size() == 3);
    assert(events[0] == ftxui::Event::Escape);
    assert(events[1] == ftxui::Event::Escape);
    assert(events[2] == ftxui::Event::Escape);
  }

  {
    std::vector<ftxui::Event> events;
    ftxui::TerminalInputParser parser([&events](ftxui::Event event) {
      events.push_back(std::move(event));
    });
    for (const char character : {'\x1B', '[', '1', '2', '\x1B', '[', 'A'}) {
      parser.Add(character);
    }

    assert(events.size() == 2);
    assert(events[0] == ftxui::Event::Special("\x1B[12"));
    assert(events[1] == ftxui::Event::ArrowUp);
  }

  return 0;
}
