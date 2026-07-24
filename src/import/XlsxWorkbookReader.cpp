// Inventatory - native XLSX table reader for local manufacturer catalogues.
#include "import/XlsxWorkbookReader.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace inventatory {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

std::string xmlUnescape(std::string value) {
  const std::pair<std::string_view, std::string_view> entities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& [from, to] : entities) {
    for (std::size_t offset = 0; (offset = value.find(from, offset)) != std::string::npos; offset += to.size()) {
      value.replace(offset, from.size(), to);
    }
  }
  return value;
}

std::string textElement(const std::string& xml) {
  std::string value;
  const std::regex text(R"(<t(?:\s[^>]*)?>([\s\S]*?)</t>)", std::regex::icase);
  for (std::sregex_iterator it(xml.begin(), xml.end(), text), end; it != end; ++it) value += xmlUnescape((*it)[1].str());
  return value;
}

std::string attribute(const std::string& tag, const char* name) {
  const std::regex expression(std::string("\\b") + name + R"(\s*=\s*(["'])(.*?)\1)", std::regex::icase);
  std::smatch match;
  return std::regex_search(tag, match, expression) ? xmlUnescape(match[2].str()) : std::string{};
}

bool readZipFile(mz_zip_archive& archive, const std::string& name, std::string& output) {
  const int index = mz_zip_reader_locate_file(&archive, name.c_str(), nullptr, 0);
  if (index < 0) return false;
  size_t size = 0;
  void* memory = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
  if (!memory) return false;
  output.assign(static_cast<const char*>(memory), size);
  mz_free(memory);
  return true;
}

std::size_t columnIndex(const std::string& reference) {
  std::size_t result = 0;
  for (const unsigned char character : reference) {
    if (!std::isalpha(character)) break;
    result = result * 26 + (std::toupper(character) - 'A' + 1);
  }
  return result == 0 ? 0 : result - 1;
}

std::string numericText(const std::string& number, const std::string& numberFormat) {
  // Parametric exports occasionally store a package identifier as a numeric
  // cell with an Excel format such as 0000.  Honour that presentation here;
  // otherwise leave the numeric lexical value untouched.
  if (numberFormat.empty() || numberFormat.find_first_not_of('0') != std::string::npos) return number;
  double parsed = 0.0;
  if (std::from_chars(number.data(), number.data() + number.size(), parsed).ec != std::errc{} ||
      parsed < 0 || std::floor(parsed) != parsed) return number;
  std::ostringstream out;
  out << std::setw(static_cast<int>(numberFormat.size())) << std::setfill('0') << static_cast<unsigned long long>(parsed);
  return out.str();
}

std::vector<std::string> customNumberFormats(const std::string& styles) {
  std::unordered_map<int, std::string> custom;
  const std::regex format(R"(<numFmt\b[^>]*>)", std::regex::icase);
  for (std::sregex_iterator it(styles.begin(), styles.end(), format), end; it != end; ++it) {
    const auto tag = it->str();
    const auto id = attribute(tag, "numFmtId");
    if (!id.empty()) custom[std::stoi(id)] = attribute(tag, "formatCode");
  }
  std::vector<std::string> result;
  const std::regex xf(R"(<xf\b[^>]*>)", std::regex::icase);
  for (std::sregex_iterator it(styles.begin(), styles.end(), xf), end; it != end; ++it) {
    const auto id = attribute(it->str(), "numFmtId");
    if (id.empty()) {
      result.emplace_back();
      continue;
    }
    const int numericId = std::stoi(id);
    const auto found = custom.find(numericId);
    result.push_back(found == custom.end() ? std::string{} : found->second);
  }
  return result;
}

std::size_t likelyHeaderRow(const std::vector<std::vector<std::string>>& rows) {
  std::size_t best = 0;
  std::size_t bestScore = 0;
  for (std::size_t index = 0; index < rows.size() && index < 50; ++index) {
    std::size_t populated = 0;
    std::size_t textLike = 0;
    for (const auto& value : rows[index]) {
      if (value.empty()) continue;
      ++populated;
      textLike += std::any_of(value.begin(), value.end(), [](unsigned char ch) { return std::isalpha(ch); }) ? 1 : 0;
    }
    const std::size_t score = populated * 3 + textLike;
    if (score > bestScore) {
      bestScore = score;
      best = index;
    }
  }
  return best;
}

}  // namespace

bool readXlsxCatalogueTable(const std::filesystem::path& file, const std::string& worksheetPattern,
                            CatalogueTable& table, std::string& error, std::atomic_bool* cancel,
                            const std::function<void(std::size_t, std::size_t, const std::string&)>& progress) {
  table = {};
  mz_zip_archive archive{};
  if (!mz_zip_reader_init_file(&archive, file.string().c_str(), 0)) {
    error = "The XLSX file is not a readable ZIP workbook";
    return false;
  }
  struct CloseZip { mz_zip_archive& value; ~CloseZip() { mz_zip_reader_end(&value); } } close{archive};

  std::string workbook;
  if (!readZipFile(archive, "xl/workbook.xml", workbook)) {
    error = "The XLSX workbook is missing xl/workbook.xml";
    return false;
  }
  std::string relationships;
  readZipFile(archive, "xl/_rels/workbook.xml.rels", relationships);
  std::unordered_map<std::string, std::string> relationshipTargets;
  const std::regex relationship(R"(<Relationship\b[^>]*>)", std::regex::icase);
  for (std::sregex_iterator it(relationships.begin(), relationships.end(), relationship), end; it != end; ++it) {
    relationshipTargets.emplace(attribute(it->str(), "Id"), attribute(it->str(), "Target"));
  }

  std::string selectedName;
  std::string selectedTarget;
  const auto sought = lower(worksheetPattern);
  const std::regex sheet(R"(<sheet\b[^>]*>)", std::regex::icase);
  for (std::sregex_iterator it(workbook.begin(), workbook.end(), sheet), end; it != end; ++it) {
    const auto tag = it->str();
    const auto name = attribute(tag, "name");
    const auto relationshipId = attribute(tag, "r:id");
    const auto target = relationshipTargets[relationshipId];
    if (target.empty()) continue;
    if (selectedTarget.empty() || (!sought.empty() && lower(name).find(sought) != std::string::npos)) {
      selectedName = name;
      selectedTarget = target;
      if (!sought.empty() && lower(name).find(sought) != std::string::npos) break;
    }
  }
  if (selectedTarget.empty()) {
    error = "The XLSX workbook does not contain a readable worksheet";
    return false;
  }
  while (selectedTarget.rfind("../", 0) == 0) selectedTarget.erase(0, 3);
  if (selectedTarget.rfind("xl/", 0) != 0) selectedTarget = "xl/" + selectedTarget;

  std::string sharedStringsXml;
  readZipFile(archive, "xl/sharedStrings.xml", sharedStringsXml);
  std::vector<std::string> sharedStrings;
  const std::regex shared(R"(<si\b[^>]*>([\s\S]*?)</si>)", std::regex::icase);
  for (std::sregex_iterator it(sharedStringsXml.begin(), sharedStringsXml.end(), shared), end; it != end; ++it) {
    sharedStrings.push_back(textElement((*it)[1].str()));
  }
  std::string styles;
  readZipFile(archive, "xl/styles.xml", styles);
  const auto formats = customNumberFormats(styles);

  std::string worksheet;
  if (!readZipFile(archive, selectedTarget, worksheet)) {
    error = "The selected XLSX worksheet is missing";
    return false;
  }
  table.sheetName = selectedName;
  std::vector<std::vector<std::string>> allRows;
  const std::regex row(R"(<row\b[^>]*>([\s\S]*?)</row>)", std::regex::icase);
  const std::regex cell(R"(<c\b([^>]*)>([\s\S]*?)</c>)", std::regex::icase);
  std::size_t parsedRows = 0;
  for (std::sregex_iterator rows(worksheet.begin(), worksheet.end(), row), end; rows != end; ++rows) {
    if (cancel && cancel->load()) {
      error = "Workbook import cancelled";
      return false;
    }
    std::vector<std::string> values;
    for (std::sregex_iterator cells((*rows)[1].first, (*rows)[1].second, cell), cellEnd; cells != cellEnd; ++cells) {
      const auto attributes = (*cells)[1].str();
      const auto content = (*cells)[2].str();
      const std::size_t column = columnIndex(attribute(attributes, "r"));
      if (values.size() <= column) values.resize(column + 1);
      const auto type = attribute(attributes, "t");
      std::string value;
      if (type == "s") {
        std::smatch index;
        if (std::regex_search(content, index, std::regex(R"(<v>(.*?)</v>)", std::regex::icase))) {
          const auto parsed = std::stoul(index[1].str());
          if (parsed < sharedStrings.size()) value = sharedStrings[parsed];
        }
      } else if (type == "inlineStr") {
        value = textElement(content);
      } else {
        std::smatch raw;
        if (std::regex_search(content, raw, std::regex(R"(<v>(.*?)</v>)", std::regex::icase))) value = xmlUnescape(raw[1].str());
        const auto style = attribute(attributes, "s");
        if (!style.empty()) {
          const auto styleIndex = std::stoul(style);
          if (styleIndex < formats.size()) value = numericText(value, formats[styleIndex]);
        }
      }
      values[column] = value;
    }
    allRows.push_back(std::move(values));
    ++parsedRows;
    if (progress && parsedRows % 1000 == 0) progress(parsedRows, 0, "Reading workbook");
  }
  if (allRows.empty()) {
    error = "The selected XLSX worksheet is empty";
    return false;
  }
  table.headerRow = likelyHeaderRow(allRows);
  table.headers = allRows[table.headerRow];
  table.rows.assign(allRows.begin() + static_cast<std::ptrdiff_t>(table.headerRow + 1), allRows.end());
  table.sourceRowCount = allRows.size();
  table.encoding = "UTF-8";
  return true;
}

}  // namespace inventatory
