// Inventatory - Hardware Inventory Management System
// Delimiter-agnostic CSV scanning shared by every importer.

#include "import/CsvReader.h"

#include "core/Inventory.h"

#include <cctype>

namespace inventatory {

using namespace std;

string stripByteOrderMark(string text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  return text;
}

char sniffDelimiter(const string& text) {
  size_t comma = 0;
  size_t semicolon = 0;
  size_t tab = 0;
  bool inQuotes = false;
  bool sawContent = false;

  for (const char ch : text) {
    if (ch == '"') {
      inQuotes = !inQuotes;
      sawContent = true;
      continue;
    }
    if (inQuotes) {
      sawContent = true;
      continue;
    }
    if (ch == '\n') {
      // Keep scanning while the line so far held nothing but whitespace.
      if (sawContent) {
        break;
      }
      continue;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == ',') {
      ++comma;
    } else if (ch == ';') {
      ++semicolon;
    } else if (ch == '\t') {
      ++tab;
    }
    if (!isspace(static_cast<unsigned char>(ch))) {
      sawContent = true;
    }
  }

  if (semicolon > comma && semicolon >= tab) {
    return ';';
  }
  if (tab > comma && tab >= semicolon) {
    return '\t';
  }
  return ',';
}

vector<vector<string>> parseCsv(const string& text, char delimiter, string& error) {
  vector<vector<string>> rows;
  vector<string> row;
  string field;
  bool inQuotes = false;

  for (size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];

    if (inQuotes) {
      if (ch == '"') {
        if (index + 1 < text.size() && text[index + 1] == '"') {
          field.push_back('"');
          ++index;
        } else if (index + 1 >= text.size() || text[index + 1] == delimiter || text[index + 1] == '\r' ||
                   text[index + 1] == '\n') {
          inQuotes = false;
        } else {
          // Lenient recovery: KiCad writes inch marks unescaped, as in
          // "2.13" ePaper". A quote that is not followed by a delimiter or a
          // line break cannot be closing the field, so keep it as content.
          field.push_back('"');
        }
      } else {
        field.push_back(ch);
      }
      continue;
    }

    if (ch == '"') {
      inQuotes = true;
    } else if (ch == delimiter) {
      row.push_back(trim(field));
      field.clear();
    } else if (ch == '\n') {
      row.push_back(trim(field));
      field.clear();
      if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
        rows.push_back(move(row));
      }
      row.clear();
    } else if (ch != '\r') {
      field.push_back(ch);
    }
  }

  if (inQuotes) {
    error = "CSV has an unterminated quoted field";
    return {};
  }

  row.push_back(trim(field));
  if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
    rows.push_back(move(row));
  }

  return rows;
}

string normalizeHeader(string value) {
  value = toLower(trim(value));
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(ch));
    }
  }
  return normalized;
}

bool anyHeaderMatches(const string& header, initializer_list<const char*> aliases) {
  const auto normalized = normalizeHeader(header);
  for (const auto* alias : aliases) {
    if (normalized == normalizeHeader(alias)) {
      return true;
    }
  }
  return false;
}

int findColumn(const vector<string>& headers, initializer_list<const char*> aliases) {
  for (size_t index = 0; index < headers.size(); ++index) {
    if (anyHeaderMatches(headers[index], aliases)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

string csvCell(const vector<string>& row, int index) {
  if (index < 0 || static_cast<size_t>(index) >= row.size()) {
    return {};
  }
  return trim(row[static_cast<size_t>(index)]);
}

}  // namespace inventatory
