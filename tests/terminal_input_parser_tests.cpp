#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/component/terminal_input_parser.hpp>

// The checks are assert() calls. CMake's Release configuration defines NDEBUG, which compiles every
// assert() out, so a Release ctest run would pass without checking anything. Keep the checks enabled
// in every build type.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
  {
    // Escape is also the prefix of terminal Alt/VT sequences. A burst of
    // Escape presses must remain separate events rather than becoming text.
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
