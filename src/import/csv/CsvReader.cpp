// Inventatory - Hardware Inventory Management System
// Delimiter-agnostic CSV scanning shared by every importer.

#include "import/csv/CsvReader.h"

#include "core/inventory/Inventory.h"

#include <cctype>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaximumCsvRows = 100000;
constexpr size_t kMaximumCsvFieldsPerRow = 512;
constexpr size_t kMaximumCsvFieldBytes = 1024U * 1024U;
constexpr size_t kMaximumCsvInputBytes = 25U * 1024U * 1024U;

}  // namespace

string stripByteOrderMark(string text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  return text;
}

bool isValidUtf8(const string& text) {
  size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    size_t length = 0;
    unsigned minimum = 0;
    unsigned codePoint = 0;
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
      minimum = 0x80U;
      codePoint = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
      minimum = 0x800U;
      codePoint = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
      minimum = 0x10000U;
      codePoint = lead & 0x07U;
    } else {
      return false;
    }
    if (index + length > text.size()) return false;
    for (size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      codePoint = (codePoint << 6U) | (continuation & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

namespace {

void appendUtf8(string& out, unsigned codePoint) {
  if (codePoint < 0x80U) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else if (codePoint < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  }
}

bool decodeUtf16(const string& bytes, bool littleEndian, string& utf8) {
  if ((bytes.size() - 2U) % 2U != 0U) return false;
  const auto unitAt = [&](size_t offset) {
    const auto first = static_cast<unsigned>(static_cast<unsigned char>(bytes[offset]));
    const auto second = static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 1]));
    return littleEndian ? (second << 8U) | first : (first << 8U) | second;
  };
  utf8.clear();
  utf8.reserve(bytes.size());
  for (size_t offset = 2; offset < bytes.size(); offset += 2) {
    const auto unit = unitAt(offset);
    if (unit >= 0xD800U && unit <= 0xDBFFU) {
      if (offset + 4U > bytes.size()) return false;
      const auto low = unitAt(offset + 2U);
      if (low < 0xDC00U || low > 0xDFFFU) return false;
      appendUtf8(utf8, 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U));
      offset += 2;
    } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
      return false;  // lone low surrogate
    } else {
      appendUtf8(utf8, unit);
    }
  }
  return true;
}

}  // namespace

bool decodeCsvBytes(const string& bytes, string& utf8, string& error) {
  error.clear();
  const auto byteAt = [&](size_t index) { return static_cast<unsigned char>(bytes[index]); };
  if (bytes.size() >= 2 && ((byteAt(0) == 0xFF && byteAt(1) == 0xFE) || (byteAt(0) == 0xFE && byteAt(1) == 0xFF))) {
    if (bytes.size() >= 4 && byteAt(0) == 0xFF && byteAt(1) == 0xFE && byteAt(2) == 0 && byteAt(3) == 0) {
      error = "CSV is encoded as UTF-32; save it as CSV UTF-8 and import it again";
      return false;
    }
    if (!decodeUtf16(bytes, byteAt(0) == 0xFF, utf8)) {
      error = "CSV looks like UTF-16 but is truncated or corrupt; save it as CSV UTF-8 and import it again";
      return false;
    }
    return true;
  }

  auto text = stripByteOrderMark(bytes);
  if (text.find('\0') != string::npos) {
    error = "CSV contains NUL bytes, so it is probably UTF-16 without a byte order mark; save it as CSV UTF-8 and "
            "import it again";
    return false;
  }
  if (!isValidUtf8(text)) {
    error = "CSV is not valid UTF-8 (a legacy Windows code page such as Windows-1250?); save it as CSV UTF-8 and "
            "import it again";
    return false;
  }
  utf8 = move(text);
  return true;
}

char sniffDelimiter(const string& text) {
  if (text.size() > kMaximumCsvInputBytes) {
    return ',';
  }

  size_t comma = 0;
  size_t semicolon = 0;
  size_t tab = 0;
  bool inQuotes = false;
  bool sawContent = false;
  // A quote only opens a quoted section at the start of a field; inside 2.13" ePaper it is a literal
  // inch mark and must not swallow the delimiters that follow it.
  bool atFieldStart = true;

  for (const char ch : text) {
    if (ch == '"') {
      if (inQuotes) {
        inQuotes = false;
      } else if (atFieldStart) {
        inQuotes = true;
      }
      atFieldStart = false;
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
      atFieldStart = true;
      continue;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == ',' || ch == ';' || ch == '\t') {
      if (ch == ',') ++comma;
      else if (ch == ';') ++semicolon;
      else ++tab;
      atFieldStart = true;
    } else if (!isspace(static_cast<unsigned char>(ch))) {
      atFieldStart = false;
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
  error.clear();
  if (text.size() > kMaximumCsvInputBytes) {
    error = "CSV exceeds the 25 MiB safety limit";
    return {};
  }

  vector<vector<string>> rows;
  vector<string> row;
  string field;
  bool inQuotes = false;

  const auto fieldTooLarge = [&]() {
    if (field.size() > kMaximumCsvFieldBytes) {
      error = "CSV contains a field larger than the 1 MiB safety limit";
      return true;
    }
    return false;
  };

  for (size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];

    if (inQuotes) {
      if (ch == '"') {
        if (index + 1 < text.size() && text[index + 1] == '"') {
          field.push_back('"');
          ++index;
          if (fieldTooLarge()) return {};
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
      if (fieldTooLarge()) return {};
      continue;
    }

    if (ch == '"' && field.find_first_not_of(" \t") == string::npos) {
      // A quote opens a quoted section only at the start of a field. Elsewhere (2.13" ePaper in an
      // unquoted field) it is an inch mark; treating it as an opener would swallow the delimiters and
      // line breaks that follow.
      inQuotes = true;
    } else if (ch == delimiter) {
      row.push_back(trim(field));
      field.clear();
      if (row.size() > kMaximumCsvFieldsPerRow) {
        error = "CSV row contains too many fields";
        return {};
      }
    } else if (ch == '\n') {
      row.push_back(trim(field));
      field.clear();
      if (row.size() > kMaximumCsvFieldsPerRow) {
        error = "CSV row contains too many fields";
        return {};
      }
      if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
        if (rows.size() >= kMaximumCsvRows) {
          error = "CSV contains too many rows";
          return {};
        }
        rows.push_back(move(row));
      }
      row.clear();
    } else if (ch != '\r') {
      field.push_back(ch);
      if (fieldTooLarge()) return {};
    }
  }

  if (inQuotes) {
    error = "CSV has an unterminated quoted field";
    return {};
  }

  row.push_back(trim(field));
  if (row.size() > kMaximumCsvFieldsPerRow) {
    error = "CSV row contains too many fields";
    return {};
  }
  if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
    if (rows.size() >= kMaximumCsvRows) {
      error = "CSV contains too many rows";
      return {};
    }
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
