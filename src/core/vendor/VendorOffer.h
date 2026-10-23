// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// What a vendor sells a part for right now: price breaks, stock, minimum order and multiple.
// Amounts are whole millionths of a currency unit, never floating point.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

inline constexpr std::int64_t kMicrosPerUnit = 1000000;
// Largest unit price accepted: 10,000 currency units.
inline constexpr std::int64_t kMaximumUnitMicros = 10000 * kMicrosPerUnit;
// Largest order quantity priced. With kMaximumUnitMicros the total stays inside int64.
inline constexpr std::uint64_t kMaximumOrderQuantity = 100000000;
// An offer older than this is shown as an old price and is not ranked as current.
inline constexpr std::int64_t kOfferFreshSeconds = 24 * 60 * 60;

// "0.0144" or "12" to millionths. Dot decimal only, no sign or exponent. Digits past the sixth
// decimal round half up. nullopt for anything else or for more than kMaximumUnitMicros.
std::optional<std::int64_t> parseDecimalMicros(const std::string& text);

// A price as a vendor prints it: "$0.10", "0,453 zł", "1.234,56 €", "0.02 EUR". Currency signs and
// codes around the number are ignored; the currency comes from its own field. With both separators
// the last one is the decimal point; one used several times groups thousands. One separator used
// once is the decimal point, except "1,234" or "1.234", which could be either: decimalPoint ('.' or
// ',') settles it, and without it such a price is refused. nullopt for anything not exact, such as
// "-1", "1,2,3", or a number that contradicts decimalPoint.
std::optional<std::int64_t> parsePriceTextMicros(const std::string& text, char decimalPoint = 0);

// The decimal point a price shows beyond doubt ("0,453 zł", "1.234,56", "$0.10"), or 0 when it does
// not ("12", "1,234"). Prices in one list share their format, so one clear price settles the others.
char priceDecimalPoint(const std::string& text);

// The net amount inside a gross one, for a VAT rate in percent such as "23" or "23.0". Rounds to
// the nearest millionth. nullopt for a rate outside 0 to 100 or one that is not a plain decimal.
std::optional<std::int64_t> netFromGrossMicros(std::int64_t gross, const std::string& vatRatePercent);

// True for an https link on the vendor's own site: digikey.<country>, mouser.<country> or tme.eu,
// with any subdomain, and no user name, port or unusual character. Offers keep no other link.
bool vendorLinkAllowed(const std::string& provider, const std::string& url);

// Three upper-case letters, as ISO 4217 codes are.
bool isCurrencyCode(const std::string& code);

// "0.0144" with at least two decimals and no trailing zeros past them.
std::string formatMicros(std::int64_t micros);
// A unit price for display: four decimals, or six below 0.01 so small prices keep their digits;
// rounded half up, with at least two decimals. 1816016 reads "1.816", 8520 reads "0.00852".
std::string formatPriceMicros(std::int64_t micros);

struct PriceBreak {
  std::uint64_t quantity = 0;      // Price applies from this many pieces
  std::int64_t unitMicros = 0;     // Net, in the offer's currency
};

// One vendor's offer for one orderable product number.
struct VendorOffer {
  std::string provider;            // kVendorProvider*
  std::string sku;                 // The vendor's own part number
  std::string manufacturer;
  std::string manufacturerPartNumber;
  std::string productUrl;
  std::string currency;            // ISO 4217, as the vendor reported it
  std::vector<PriceBreak> breaks;  // Ascending quantity, no duplicates; see normalizePriceBreaks
  std::optional<std::uint64_t> stock;  // Empty when the vendor did not say
  std::uint64_t minimumOrder = 1;
  std::uint64_t multiple = 1;
  std::string packaging;           // "Cut Tape (CT)", "Tape & Reel (TR)", ...
  std::optional<std::int64_t> reelingFeeMicros;  // Added once per order line (DigiKey Digi-Reel)
  std::string lifecycle;           // As the vendor wrote it; empty when not given
  bool orderable = true;
  std::time_t fetchedAt = 0;
};

// Sorts by quantity and drops zero quantities, non-positive prices and repeated quantities (the
// first one wins). Returns false when nothing usable remains.
bool normalizePriceBreaks(std::vector<PriceBreak>& breaks);

// True when the offer was fetched no more than kOfferFreshSeconds before now, and not in the future.
bool offerIsFresh(const VendorOffer& offer, std::time_t now);

// What buying a quantity from an offer costs.
struct OfferCost {
  bool priced = false;             // False: no break applies or the order is too large
  std::uint64_t orderQuantity = 0; // Needed, raised to the minimum and a whole multiple
  std::uint64_t extra = 0;         // orderQuantity - needed
  std::int64_t unitMicros = 0;
  std::int64_t totalMicros = 0;    // orderQuantity * unit, plus the reeling fee
};

// The cost of at least `needed` pieces: the order is raised to the minimum order and rounded up to
// the multiple, and priced at the largest break at or below it. Not priced when needed is 0.
OfferCost offerCost(const VendorOffer& offer, std::uint64_t needed);

// A larger order that costs less in total than offerCost(offer, needed), such as 100 pieces costing
// less than 90. The cheapest such break, or nullopt when buying the needed amount is cheapest.
std::optional<OfferCost> cheaperLargerOrder(const VendorOffer& offer, std::uint64_t needed);

// One offer as two stock detail lines: the vendor part number and its price, then the facts and the
// status. Every vendor fills both lines the same way.
enum class OfferStatus { None, OldPrice, OtherCurrency, NotSold };
struct OfferLine {
  std::string sku;         // The vendor's part number, which every vendor sends
  std::string price;       // Lowest-quantity unit price for display: "0.0046 EUR"
  std::string facts;       // "0 in stock, Digi-Reel, from 10000 pcs, reel fee 5.50 EUR"
  OfferStatus status = OfferStatus::None;
  std::string statusWord;  // "Old price", "Other currency", "Not sold", or empty
};
// The status is the most serious one: not sold, then another currency than the shared one (when one
// is set), then a price older than kOfferFreshSeconds.
OfferLine describeOffer(const VendorOffer& offer, const std::string& sharedCurrency, std::time_t now);

}  // namespace inventatory
