// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/vendor/VendorOffer.h"

#include <algorithm>
#include <cctype>

namespace inventatory {
namespace {

using std::int64_t;
using std::optional;
using std::string;
using std::uint64_t;

constexpr std::size_t kMaximumIntegerDigits = 12;

bool isDigit(char character) { return std::isdigit(static_cast<unsigned char>(character)) != 0; }

// Spaces a vendor may put inside or around a price: ASCII, no-break (U+00A0) and narrow no-break
// (U+202F). Returns the byte length of the space at position, or 0.
std::size_t spaceLength(const string& text, std::size_t position) {
  if (text[position] == ' ' || text[position] == '\t') return 1;
  if (text.compare(position, 2, "\xC2\xA0") == 0) return 2;
  if (text.compare(position, 3, "\xE2\x80\xAF") == 0) return 3;
  return 0;
}

// Thousands groups after the first are exactly three digits: "1,234,567".
bool validGroups(const string& digits, char separator) {
  std::size_t start = 0;
  bool first = true;
  while (true) {
    const auto end = digits.find(separator, start);
    const auto length = (end == string::npos ? digits.size() : end) - start;
    if (length == 0 || length > 3 || (!first && length != 3)) return false;
    if (end == string::npos) return true;
    first = false;
    start = end + 1;
  }
}

string withoutSeparator(string text, char separator) {
  text.erase(std::remove(text.begin(), text.end(), separator), text.end());
  return text;
}

}  // namespace

optional<int64_t> parseDecimalMicros(const string& text) {
  const auto point = text.find('.');
  const auto integer = text.substr(0, point);
  const auto fraction = point == string::npos ? string() : text.substr(point + 1);
  if (integer.empty() || integer.size() > kMaximumIntegerDigits) return std::nullopt;
  if (point != string::npos && fraction.empty()) return std::nullopt;
  if (!std::all_of(integer.begin(), integer.end(), isDigit) || !std::all_of(fraction.begin(), fraction.end(), isDigit)) {
    return std::nullopt;
  }
  int64_t micros = 0;
  for (const char digit : integer) micros = micros * 10 + (digit - '0');
  int64_t fractional = 0;
  for (std::size_t index = 0; index < 6; ++index) {
    fractional = fractional * 10 + (index < fraction.size() ? fraction[index] - '0' : 0);
  }
  micros = micros * kMicrosPerUnit + fractional;
  if (fraction.size() > 6 && fraction[6] >= '5') ++micros;
  if (micros > kMaximumUnitMicros) return std::nullopt;
  return micros;
}

namespace {

// The run from the first digit to the last, with each space between digits as one ' '. nullopt for
// a sign, a separator just before the first digit (".5"), or any other character inside the run.
optional<string> priceNumber(const string& text) {
  if (text.find('-') != string::npos || text.find('+') != string::npos) return std::nullopt;
  std::size_t first = 0;
  while (first < text.size() && !isDigit(text[first])) ++first;
  if (first == text.size()) return std::nullopt;
  std::size_t last = text.size() - 1;
  while (!isDigit(text[last])) --last;
  if (first > 0 && (text[first - 1] == '.' || text[first - 1] == ',')) return std::nullopt;
  string number;
  for (std::size_t position = first; position <= last;) {
    const char character = text[position];
    if (isDigit(character) || character == '.' || character == ',') {
      number += character;
      ++position;
    } else if (const auto length = spaceLength(text, position); length > 0) {
      number += ' ';
      position += length;
    } else {
      return std::nullopt;
    }
  }
  return number;
}

// "1,234" could be one thousand or one point two: one separator, three digits after it, and a
// whole part that is not zero. "0,453" and "1,5" are clear.
bool unclearSeparator(const string& number, std::size_t position) {
  return number.size() - position - 1 == 3 && number.substr(0, position) != "0";
}

}  // namespace

char priceDecimalPoint(const string& text) {
  const auto number = priceNumber(text);
  if (!number.has_value()) return 0;
  const auto commas = std::count(number->begin(), number->end(), ',');
  const auto dots = std::count(number->begin(), number->end(), '.');
  if (commas > 0 && dots > 0) return number->rfind(',') > number->rfind('.') ? ',' : '.';
  if (commas + dots == 0) return 0;
  const char separator = commas > 0 ? ',' : '.';
  // Used several times, it groups thousands, so the other one is the decimal point.
  if (commas + dots > 1) return separator == ',' ? '.' : ',';
  return unclearSeparator(*number, number->find(separator)) ? 0 : separator;
}

optional<int64_t> parsePriceTextMicros(const string& text, char decimalPoint) {
  if (decimalPoint != 0 && decimalPoint != '.' && decimalPoint != ',') return std::nullopt;
  const auto parsed = priceNumber(text);
  if (!parsed.has_value()) return std::nullopt;
  auto number = *parsed;
  // With spaces grouping thousands, the one comma or dot left is the decimal point.
  if (number.find(' ') != string::npos) {
    const auto decimal = number.find_first_of(",.");
    if (decimal != string::npos && number.find_first_of(",.", decimal + 1) != string::npos) return std::nullopt;
    if (decimal != string::npos && decimalPoint != 0 && number[decimal] != decimalPoint) return std::nullopt;
    const auto integer = decimal == string::npos ? number : number.substr(0, decimal);
    if (!validGroups(integer, ' ')) return std::nullopt;
    number = withoutSeparator(integer, ' ') + (decimal == string::npos ? string() : "." + number.substr(decimal + 1));
    return parseDecimalMicros(number);
  }
  const auto commas = std::count(number.begin(), number.end(), ',');
  const auto dots = std::count(number.begin(), number.end(), '.');
  if (commas > 0 && dots > 0) {
    const char decimal = number.rfind(',') > number.rfind('.') ? ',' : '.';
    if (decimalPoint != 0 && decimal != decimalPoint) return std::nullopt;
    const char grouping = decimal == ',' ? '.' : ',';
    const auto position = number.rfind(decimal);
    if (std::count(number.begin(), number.end(), decimal) != 1) return std::nullopt;
    const auto integer = number.substr(0, position);
    if (!validGroups(integer, grouping)) return std::nullopt;
    return parseDecimalMicros(withoutSeparator(integer, grouping) + "." + number.substr(position + 1));
  }
  const char separator = commas > 0 ? ',' : '.';
  const auto count = commas > 0 ? commas : dots;
  if (count == 0) return parseDecimalMicros(number);
  const bool isDecimal = decimalPoint != 0 ? separator == decimalPoint : count == 1;
  if (isDecimal) {
    if (count != 1) return std::nullopt;
    // Without a hint, an unclear "1,234" is refused rather than read a thousand times too low.
    if (decimalPoint == 0 && unclearSeparator(number, number.find(separator))) return std::nullopt;
    std::replace(number.begin(), number.end(), separator, '.');
    return parseDecimalMicros(number);
  }
  if (!validGroups(number, separator)) return std::nullopt;
  return parseDecimalMicros(withoutSeparator(number, separator));
}

optional<int64_t> netFromGrossMicros(int64_t gross, const string& vatRatePercent) {
  if (gross < 0 || gross > kMaximumUnitMicros) return std::nullopt;
  const auto rate = parseDecimalMicros(vatRatePercent);
  if (!rate.has_value() || *rate > 100 * kMicrosPerUnit) return std::nullopt;
  const int64_t hundred = 100 * kMicrosPerUnit;
  const int64_t denominator = hundred + *rate;
  return (gross * hundred + denominator / 2) / denominator;
}

bool vendorLinkAllowed(const string& provider, const string& url) {
  constexpr std::size_t kMaximumLinkBytes = 2048;
  const string scheme = "https://";
  if (url.size() > kMaximumLinkBytes || url.compare(0, scheme.size(), scheme) != 0) return false;
  for (const unsigned char character : url) {
    if (character <= 0x20U || character >= 0x7fU || character == '\\' || character == '"' || character == '<' ||
        character == '>' || character == '|' || character == '`') {
      return false;
    }
  }
  const auto hostEnd = url.find_first_of("/?#", scheme.size());
  const auto host = url.substr(scheme.size(), hostEnd == string::npos ? string::npos : hostEnd - scheme.size());
  if (host.empty() || host.find_first_of("@:") != string::npos) return false;
  for (const char character : host) {
    if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9') && character != '.' &&
        character != '-') {
      return false;
    }
  }
  // Labels of the host, last one first.
  std::vector<string> labels;
  for (std::size_t start = 0;;) {
    const auto dot = host.find('.', start);
    labels.push_back(host.substr(start, dot == string::npos ? string::npos : dot - start));
    if (dot == string::npos) break;
    start = dot + 1;
  }
  if (std::any_of(labels.begin(), labels.end(), [](const string& label) { return label.empty(); })) return false;
  std::reverse(labels.begin(), labels.end());
  if (provider == "tme") return labels.size() >= 2 && labels[0] == "eu" && labels[1] == "tme";
  if (provider != "digikey" && provider != "mouser") return false;
  // A country ending ("de", "pl", "com") or a two-part one ("co.uk", "com.au"), then the brand.
  const auto shortLabel = [](const string& label) { return label.size() >= 2 && label.size() <= 3; };
  if (labels.size() >= 2 && shortLabel(labels[0]) && labels[1] == provider) return true;
  return labels.size() >= 3 && labels[0].size() == 2 && shortLabel(labels[1]) && labels[2] == provider;
}

bool isCurrencyCode(const string& code) {
  return code.size() == 3 &&
         std::all_of(code.begin(), code.end(), [](char character) { return character >= 'A' && character <= 'Z'; });
}

string formatMicros(int64_t micros) {
  const bool negative = micros < 0;
  const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(micros) : static_cast<uint64_t>(micros);
  auto fraction = std::to_string(magnitude % kMicrosPerUnit);
  fraction.insert(0, 6 - fraction.size(), '0');
  while (fraction.size() > 2 && fraction.back() == '0') fraction.pop_back();
  return (negative ? "-" : "") + std::to_string(magnitude / kMicrosPerUnit) + "." + fraction;
}

string formatPriceMicros(int64_t micros) {
  if (micros < 0) return formatMicros(micros);
  if (micros >= 10000) micros = (micros + 50) / 100 * 100;
  return formatMicros(micros);
}

bool normalizePriceBreaks(std::vector<PriceBreak>& breaks) {
  breaks.erase(std::remove_if(breaks.begin(), breaks.end(),
                              [](const PriceBreak& entry) { return entry.quantity == 0 || entry.unitMicros <= 0; }),
               breaks.end());
  std::stable_sort(breaks.begin(), breaks.end(),
                   [](const PriceBreak& left, const PriceBreak& right) { return left.quantity < right.quantity; });
  breaks.erase(std::unique(breaks.begin(), breaks.end(),
                           [](const PriceBreak& left, const PriceBreak& right) { return left.quantity == right.quantity; }),
               breaks.end());
  return !breaks.empty();
}

bool offerIsFresh(const VendorOffer& offer, std::time_t now) {
  if (offer.fetchedAt <= 0 || offer.fetchedAt > now) return false;
  return static_cast<int64_t>(now - offer.fetchedAt) <= kOfferFreshSeconds;
}

bool lifecycleIsObsolete(const string& lifecycle) {
  // ASCII letters only, so the match does not depend on the locale.
  string lowered;
  for (const char character : lifecycle) {
    lowered += (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
  }
  return lowered.find("obsolete") != string::npos;
}

bool offerIsObsolete(const VendorOffer& offer) { return lifecycleIsObsolete(offer.lifecycle); }

OfferCost offerCost(const VendorOffer& offer, uint64_t needed) {
  OfferCost cost;
  if (needed == 0 || needed > kMaximumOrderQuantity) return cost;
  const uint64_t multiple = std::max<uint64_t>(offer.multiple, 1);
  uint64_t order = std::max(needed, offer.minimumOrder);
  if (order % multiple != 0) order += multiple - order % multiple;
  if (order > kMaximumOrderQuantity) return cost;
  const PriceBreak* applied = nullptr;
  for (const auto& entry : offer.breaks) {
    if (entry.quantity <= order && (applied == nullptr || entry.quantity > applied->quantity)) applied = &entry;
  }
  if (applied == nullptr || applied->unitMicros <= 0 || applied->unitMicros > kMaximumUnitMicros) return cost;
  const int64_t fee = offer.reelingFeeMicros.value_or(0);
  if (fee < 0 || fee > kMaximumUnitMicros) return cost;
  cost.priced = true;
  cost.orderQuantity = order;
  cost.extra = order - needed;
  cost.unitMicros = applied->unitMicros;
  cost.totalMicros = static_cast<int64_t>(order) * applied->unitMicros + fee;
  return cost;
}

optional<OfferCost> cheaperLargerOrder(const VendorOffer& offer, uint64_t needed) {
  const auto base = offerCost(offer, needed);
  if (!base.priced) return std::nullopt;
  optional<OfferCost> best;
  for (const auto& entry : offer.breaks) {
    if (entry.quantity <= base.orderQuantity) continue;
    const auto larger = offerCost(offer, entry.quantity);
    if (!larger.priced || larger.totalMicros >= base.totalMicros) continue;
    if (!best.has_value() || larger.totalMicros < best->totalMicros) {
      best = larger;
      best->extra = larger.orderQuantity - needed;
    }
  }
  return best;
}

OfferLine describeOffer(const VendorOffer& offer, const string& sharedCurrency, std::time_t now) {
  OfferLine line;
  line.sku = offer.sku;
  line.facts = offer.stock.has_value() ? std::to_string(*offer.stock) + " in stock" : string("stock unknown");
  // "Cut Tape (CT)" reads as "Cut Tape"; non-ASCII marks such as a registered sign are dropped.
  string packaging;
  int depth = 0;
  for (const char character : offer.packaging) {
    if (character == '(') ++depth;
    if (depth == 0 && static_cast<unsigned char>(character) < 0x80U) packaging += character;
    if (character == ')' && depth > 0) --depth;
  }
  while (!packaging.empty() && packaging.back() == ' ') packaging.pop_back();
  while (!packaging.empty() && packaging.front() == ' ') packaging.erase(packaging.begin());
  if (!packaging.empty()) line.facts += ", " + packaging;
  if (!offer.breaks.empty()) {
    const auto& first = offer.breaks.front();
    line.price = formatPriceMicros(first.unitMicros) + " " + offer.currency;
    const auto from = std::max(first.quantity, offer.minimumOrder);
    if (from > 1) line.facts += ", from " + std::to_string(from) + " pcs";
  }
  if (offer.reelingFeeMicros.has_value()) {
    line.facts += ", reel fee " + formatMicros(*offer.reelingFeeMicros) + " " + offer.currency;
  }
  if (!offer.orderable) {
    line.status = OfferStatus::NotSold;
    line.statusWord = "Not sold";
  } else if (!sharedCurrency.empty() && offer.currency != sharedCurrency) {
    line.status = OfferStatus::OtherCurrency;
    line.statusWord = "Other currency";
  } else if (offerIsObsolete(offer)) {
    line.status = OfferStatus::EndOfLife;
    line.statusWord = "End of life";
  } else if (!offerIsFresh(offer, now)) {
    line.status = OfferStatus::OldPrice;
    line.statusWord = "Old price";
  }
  return line;
}

}  // namespace inventatory
