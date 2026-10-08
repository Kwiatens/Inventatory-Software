// Inventatory - update wizard presentation contracts.

#pragma once

#include "platform/system/Console.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

struct UpdatePreviewControlHints {
  const char* title = "Inventatory Update Available";
  const char* releaseNotesHeading = "Release notes";
  const char* start = "Enter";
  const char* cancel = "Esc";
  const char* readNotes = "Up/Down read notes";
};

inline UpdatePreviewControlHints updatePreviewControlHints() {
  return {};
}

inline std::string updatePreviewVersionLine(const std::string& installedVersion,
                                            const std::string& newVersion) {
  return installedVersion + " -> " + newVersion;
}

inline bool updatePreviewStartsOnKey(const KeyEvent& key) {
  return key.type == KeyType::Enter;
}

enum class UpdateNoteLineKind {
  Heading,
  BulletStart,
  BulletContinuation,
  Paragraph,
  Empty,
};

struct UpdateNoteLine {
  UpdateNoteLineKind kind = UpdateNoteLineKind::Paragraph;
  std::string text;

  bool operator==(const UpdateNoteLine& other) const {
    return kind == other.kind && text == other.text;
  }
};

inline std::string cleanMarkdownInline(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '[') {
      const auto closeBracket = input.find(']', i + 1);
      if (closeBracket != std::string::npos && closeBracket + 1 < input.size() && input[closeBracket + 1] == '(') {
        const auto closeParen = input.find(')', closeBracket + 2);
        if (closeParen != std::string::npos) {
          const auto label = input.substr(i + 1, closeBracket - (i + 1));
          output += cleanMarkdownInline(label);
          i = closeParen;
          continue;
        }
      }
    }
    if ((input[i] == '*' && i + 1 < input.size() && input[i + 1] == '*') ||
        (input[i] == '_' && i + 1 < input.size() && input[i + 1] == '_') ||
        (input[i] == '~' && i + 1 < input.size() && input[i + 1] == '~')) {
      ++i;
      continue;
    }
    if (input[i] == '`') {
      continue;
    }
    output.push_back(input[i]);
  }
  return output;
}

inline std::vector<UpdateNoteLine> formatUpdateNotes(const std::string& notes, int width) {
  std::vector<UpdateNoteLine> result;
  if (notes.empty()) {
    result.push_back({UpdateNoteLineKind::Paragraph, "No release notes were provided for this release."});
    return result;
  }
  const int safeWidth = std::max(20, width);
  std::istringstream input(notes);
  std::string sourceLine;
  bool previousWasEmpty = false;

  while (std::getline(input, sourceLine)) {
    if (!sourceLine.empty() && sourceLine.back() == '\r') sourceLine.pop_back();

    size_t firstNonSpace = 0;
    while (firstNonSpace < sourceLine.size() &&
           std::isspace(static_cast<unsigned char>(sourceLine[firstNonSpace])) != 0) {
      ++firstNonSpace;
    }

    if (firstNonSpace >= sourceLine.size()) {
      if (!previousWasEmpty && !result.empty()) {
        result.push_back({UpdateNoteLineKind::Empty, ""});
        previousWasEmpty = true;
      }
      continue;
    }

    previousWasEmpty = false;

    // Headings: 1 to 6 '#' followed by a space
    if (sourceLine[firstNonSpace] == '#') {
      size_t hashCount = 0;
      while (firstNonSpace + hashCount < sourceLine.size() &&
             sourceLine[firstNonSpace + hashCount] == '#') {
        ++hashCount;
      }
      if (hashCount >= 1 && hashCount <= 6 &&
          firstNonSpace + hashCount < sourceLine.size() &&
          sourceLine[firstNonSpace + hashCount] == ' ') {
        size_t textStart = firstNonSpace + hashCount;
        while (textStart < sourceLine.size() && sourceLine[textStart] == ' ') {
          ++textStart;
        }
        std::string heading = sourceLine.substr(textStart);
        while (!heading.empty() && std::isspace(static_cast<unsigned char>(heading.back())) != 0) {
          heading.pop_back();
        }
        const auto cleaned = cleanMarkdownInline(heading);
        const auto wrapped = wrapText(cleaned, std::max(10, safeWidth - 4));
        for (const auto& w : wrapped) {
          result.push_back({UpdateNoteLineKind::Heading, w});
        }
        continue;
      }
    }

    // Bullet items: '-', '*', or '+' followed by a space
    const char marker = sourceLine[firstNonSpace];
    if ((marker == '-' || marker == '*' || marker == '+') &&
        firstNonSpace + 1 < sourceLine.size() &&
        sourceLine[firstNonSpace + 1] == ' ') {
      size_t textStart = firstNonSpace + 2;
      while (textStart < sourceLine.size() && sourceLine[textStart] == ' ') {
        ++textStart;
      }
      std::string item = sourceLine.substr(textStart);
      while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())) != 0) {
        item.pop_back();
      }
      const auto cleaned = cleanMarkdownInline(item);
      const auto wrapped = wrapText(cleaned, std::max(10, safeWidth - 6));
      for (size_t i = 0; i < wrapped.size(); ++i) {
        if (i == 0) {
          result.push_back({UpdateNoteLineKind::BulletStart, wrapped[i]});
        } else {
          result.push_back({UpdateNoteLineKind::BulletContinuation, wrapped[i]});
        }
      }
      continue;
    }

    // Regular paragraph lines
    std::string text = sourceLine.substr(firstNonSpace);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
      text.pop_back();
    }
    const auto cleaned = cleanMarkdownInline(text);
    const auto wrapped = wrapText(cleaned, std::max(10, safeWidth - 4));
    for (const auto& w : wrapped) {
      result.push_back({UpdateNoteLineKind::Paragraph, w});
    }
  }

  while (!result.empty() && result.back().kind == UpdateNoteLineKind::Empty) {
    result.pop_back();
  }

  if (result.empty()) {
    result.push_back({UpdateNoteLineKind::Paragraph, ""});
  }

  return result;
}

}  // namespace inventatory
