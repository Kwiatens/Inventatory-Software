// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - which saved vendor offer to buy a quantity from.

#pragma once

#include "core/vendor/VendorOffer.h"

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

struct PurchaseChoice {
  VendorOffer offer;
  OfferCost cost;
  bool enoughStock = true;  // False when the vendor reports less stock than the order needs
  bool oldPrice = false;    // The price is older than kOfferFreshSeconds
  bool notPreferred = false;  // A vendor was preferred, and this offer is from another one
  // A cheaper offer in stock at another vendor than the one chosen, when there is one.
  bool cheaperElsewhere = false;
  VendorOffer cheaperOffer;
  OfferCost cheaperCost;
};

// The cheapest way to buy `needed` pieces from the offers. Offers that cannot be ordered, have no price
// for the quantity, or are in another currency than `currency` (when one is given) are left out, since
// their prices cannot be compared. Offers with enough stock come first. Then the preferred vendor's
// offers (when one is given) come before the others, offers the maker no longer makes come after the rest,
// the order total decides next, and the larger stock after that. The choice says when another vendor has the part cheaper in stock. nullopt when no offer
// qualifies or `needed` is 0.
std::optional<PurchaseChoice> chooseBestOffer(const std::vector<VendorOffer>& offers, std::uint64_t needed,
                                              const std::string& currency, std::time_t now,
                                              const std::string& preferredProvider = {});

// The order that buys `needed` pieces from one offer now, or nullopt when it cannot be bought that way. The
// vendor must have stock for the whole order, minimum and multiple included, and the total must be in
// proportion: up to 20 in the offer's currency is always fine, beyond that at most ten times what the needed
// pieces cost. A 12000-piece reel for two capacitors is refused, and so is an offer whose stock is unknown.
std::optional<OfferCost> orderableOffer(const VendorOffer& offer, std::uint64_t needed);

// The page to open for an offer. TME gets its search for the part number, which works when a product
// page is missing or renamed; other vendors keep their own link.
std::string vendorPurchaseUrl(const VendorOffer& offer);

// DigiKey's search for a part number. It finds the part when DigiKey stored no product page for it. Empty when the
// number is empty; the caller trims it.
std::string digiKeySearchUrl(const std::string& partNumber);

}  // namespace inventatory
