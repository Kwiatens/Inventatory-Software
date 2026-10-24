// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/vendor/VendorPurchase.h"

#include <algorithm>
#include <cctype>

namespace inventatory {

using namespace std;

namespace {

// An order costing up to this many micros (20 in the offer's currency) is always fine, so cheap parts sold
// in packs are not refused. A larger order may cost at most kMaximumOrderMultiple times the needed pieces.
constexpr int64_t kAlwaysFineOrderMicros = 20000000;
constexpr double kMaximumOrderMultiple = 10;

}  // namespace

optional<OfferCost> orderableOffer(const VendorOffer& offer, uint64_t needed) {
  if (!offer.orderable || !offer.stock.has_value()) return nullopt;
  const auto cost = offerCost(offer, needed);
  if (!cost.priced || cost.orderQuantity > *offer.stock) return nullopt;
  const auto proportionate = static_cast<double>(cost.unitMicros) * static_cast<double>(needed) * kMaximumOrderMultiple;
  if (static_cast<double>(cost.totalMicros) > max(static_cast<double>(kAlwaysFineOrderMicros), proportionate)) {
    return nullopt;
  }
  return cost;
}

optional<PurchaseChoice> chooseBestOffer(const vector<VendorOffer>& offers, uint64_t needed, const string& currency,
                                         time_t now, const string& preferredProvider) {
  if (needed == 0) return nullopt;
  vector<PurchaseChoice> choices;
  for (const auto& offer : offers) {
    if (!offer.orderable || offer.breaks.empty()) continue;
    if (!currency.empty() && offer.currency != currency) continue;
    const auto cost = offerCost(offer, needed);
    if (!cost.priced) continue;
    PurchaseChoice choice;
    choice.offer = offer;
    choice.cost = cost;
    choice.enoughStock = !offer.stock.has_value() || *offer.stock >= cost.orderQuantity;
    choice.oldPrice = !offerIsFresh(offer, now);
    choice.notPreferred = !preferredProvider.empty() && offer.provider != preferredProvider;
    choices.push_back(move(choice));
  }
  if (choices.empty()) return nullopt;
  const auto better = [](const PurchaseChoice& a, const PurchaseChoice& b) {
    if (a.enoughStock != b.enoughStock) return a.enoughStock;
    if (a.notPreferred != b.notPreferred) return !a.notPreferred;
    if (offerIsObsolete(a.offer) != offerIsObsolete(b.offer)) return !offerIsObsolete(a.offer);
    if (a.cost.totalMicros != b.cost.totalMicros) return a.cost.totalMicros < b.cost.totalMicros;
    return a.offer.stock.value_or(0) > b.offer.stock.value_or(0);
  };
  size_t best = 0;
  for (size_t index = 1; index < choices.size(); ++index) {
    if (better(choices[index], choices[best])) best = index;
  }
  auto chosen = choices[best];
  // Only an offer that is in stock can be a real saving.
  const PurchaseChoice* cheaper = nullptr;
  for (const auto& other : choices) {
    if (other.offer.provider == chosen.offer.provider || !other.enoughStock) continue;
    // An obsolete offer is never a saving over one the maker still makes.
    if (offerIsObsolete(other.offer) && !offerIsObsolete(chosen.offer)) continue;
    if (other.cost.totalMicros >= chosen.cost.totalMicros) continue;
    if (cheaper == nullptr || other.cost.totalMicros < cheaper->cost.totalMicros) cheaper = &other;
  }
  if (cheaper != nullptr) {
    chosen.cheaperElsewhere = true;
    chosen.cheaperOffer = cheaper->offer;
    chosen.cheaperCost = cheaper->cost;
  }
  return chosen;
}

// Percent-encodes every byte but ASCII letters, digits and - _ . ~, so the text is safe in a query. The checks are
// spelled out, since isalnum follows the locale.
static string percentEncode(const string& text) {
  static const char* hex = "0123456789ABCDEF";
  string encoded;
  for (const unsigned char ch : text) {
    const bool letterOrDigit = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (letterOrDigit || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded += static_cast<char>(ch);
    } else {
      encoded += '%';
      encoded += hex[ch >> 4];
      encoded += hex[ch & 15];
    }
  }
  return encoded;
}

string vendorPurchaseUrl(const VendorOffer& offer) {
  if (offer.provider != "tme" || offer.sku.empty()) return offer.productUrl;
  return "https://www.tme.eu/en/katalog/?search=" + percentEncode(offer.sku);
}

string digiKeySearchUrl(const string& partNumber) {
  if (partNumber.empty()) return {};
  return "https://www.digikey.com/en/products/result?keywords=" + percentEncode(partNumber);
}

}  // namespace inventatory
