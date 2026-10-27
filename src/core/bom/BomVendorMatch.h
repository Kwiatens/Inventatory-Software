// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Finds the part a BOM line needs in a vendor's catalogue. A line is read into a spec (what kind of
// part, its value, package and any part numbers), searched with words the vendor's search answers
// well, and every product the search returns is verified against the spec by its parameters, never by
// its text: the search is fuzzy, so "19K" also returns 619k and 6.19k. The verified products are
// ranked: active and sold by the vendor itself, in stock, the closest part number, then the cheapest
// order for the quantity needed.
//
// Parameter labels are DigiKey's ("Resistance", "Package / Case"). A vendor whose labels differ maps
// its products onto these before matching. Mouser sends none of them as attributes, so its values are read from
// the description (see platform/mouser/MouserDescription.h).

#pragma once

#include "core/bom/BomProjectStore.h"
#include "core/inventory/Inventory.h"
#include "core/vendor/VendorOffer.h"
#include "import/kicad/KicadBom.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

enum class BomPartKind {
  Resistor,
  Capacitor,     // ceramic chip
  Electrolytic,  // radial can
  Inductor,
  Crystal,
  Fuse,
  Led,
  PartNumber,  // the line names a part number, in its designation or its footprint
  Generic,     // headers, sockets, tactile switches: many parts fit, the user picks one
};

struct BomPartSpec {
  BomPartKind kind = BomPartKind::PartNumber;
  std::optional<double> value;    // ohm, farad, henry, hertz or ampere
  std::optional<double> voltage;  // rated voltage the designation asks for ("100uF 16V")
  std::string dielectric;         // "X7R", when the designation names one
  std::string chipCode;           // imperial chip size from the footprint: "0603"
  bool axial = false;             // through-hole axial body ("R_Axial_DIN0207")
  std::string icFamily;           // "SOIC", "SOT-23", "TSSOP"
  int icPins = 0;                 // 8 for SOIC-8; 0 when the family fixes the pins
  std::optional<double> diameterMm;  // radial can or cylindrical crystal
  std::string crystalSize;        // "3.20mm x 2.50mm"
  std::string crystalCode;        // "3225", the same size as vendors search it
  bool hc49 = false;
  int positions = 0;              // pin count of a header, socket or terminal block
  double pitchMm = 0;
  std::string colour;             // LED colour, lower case
  std::vector<std::string> partNumbers;  // the line's MPN, then designation, then footprint tokens
  std::string seriesCode;         // "SKRK" from "SW_Push_SPST_NO_Alps_SKRK": any variant fits
};

BomPartSpec bomPartSpec(const BomLine& line);

// One product from a vendor's search answer, as matching reads it.
struct VendorCandidate {
  std::string manufacturer;
  std::string manufacturerPartNumber;
  std::string description;
  std::string category;
  std::string status;  // "Active", "Obsolete", ...
  bool discontinued = false;
  std::vector<Parameter> parameters;
  std::optional<std::uint64_t> stock;
  std::vector<VendorOffer> offers;  // the vendor's own offers; marketplace sellers are left out
  std::string productUrl;
};

// Text of the parameter with exactly this label, or empty.
std::string vendorParameter(const VendorCandidate& candidate, const std::string& label);
// "10 kOhms", "280 mOhms", "100 µF", "10000 pF", "32.768 kHz", "500 mA", "16V" in base units, when
// the text starts with a number followed by an SI prefix and `unit`.
std::optional<double> parseVendorQuantity(const std::string& text, const std::string& unit);
// The candidate's stock: the product total, else the largest stock of one of its offers.
std::uint64_t vendorCandidateStock(const VendorCandidate& candidate);
// The cheapest order of `quantity` across the candidate's offers that can be bought now (see orderableOffer).
std::optional<OfferCost> vendorCandidateCost(const VendorCandidate& candidate, std::uint64_t quantity);
// True when the maker has stopped making the candidate's part. It can still be bought while stock lasts, so the
// picker keeps it and marks it End of life.
bool candidateEndOfLife(const VendorCandidate& candidate);

// The standard value nearest to `value`: E96 and E24 for resistors, E24 otherwise. Nullopt when the
// value already is one.
std::optional<double> nearestStandardValue(double value, BomPartKind kind);

// The answer for one line at one vendor.
struct BomVendorAnswer {
  BomLookupOutcome outcome = BomLookupOutcome::Failed;
  std::optional<VendorCandidate> part;  // Found only
  bool verified = false;  // the part was checked against the line's value, package or part number
  bool check = false;     // a variant was chosen for the user, or a value moved: the user should look
  bool needsChoice = false;  // NoMatch because many parts fit and the user picks one
  std::string note;       // why it is flagged or missing: "nearest value 19.1k (+0.5%)", "variants: N4, N8"
  int requests = 0;
};

// One keyword search at the vendor. Nullopt when the request failed (transport, limit, parse); an
// empty list when the vendor answered with nothing.
using BomVendorSearch = std::function<std::optional<std::vector<VendorCandidate>>(const std::string& keywords)>;

// Searches with the vendor's phrasing (DigiKey's for now) and returns the best verified part. Spends
// at most `maxRequests` searches; most lines take one. A failed search ends the lookup as Failed so the
// line is tried again later.
BomVendorAnswer matchBomLineAtVendor(const BomLine& line, std::uint64_t quantity, const BomVendorSearch& search,
                                     int maxRequests = 4);

// The words to search when the user picks a part for the line by hand.
std::string bomPickerQuery(const BomLine& line);

// The searches for a picker lookup. Words the user typed are searched as they are, unless they name a
// value of the line's kind: then the value is searched in the vendor's wording, as an automatic match
// does. With no words typed, a value line has two searches, since the vendor's search answers each
// wording differently.
std::vector<std::string> bomPickerQueries(const BomLine& line, const std::string& typed);
// The value the typed words name for the line's kind, as resistors are written: "R280" is 0.28 ohm and
// "280R" is 280 ohm. Nullopt when the words name no value of that kind.
std::optional<double> bomPickerTypedValue(const BomLine& line, const std::string& typed);
// The typed value in words for a status line: "0.28 ohm", "2.2 kohm". Empty when the words name no value.
std::string bomPickerReading(const BomLine& line, const std::string& typed);
// Whether a product from the picker's search fits the line, by the same checks an automatic match makes.
bool bomPickerFits(const BomLine& line, const VendorCandidate& candidate);

struct BomPickerEntry {
  VendorCandidate candidate;
  bool fits = false;  // verified against the line, as an automatic match would be
};
struct BomPickerRanking {
  std::vector<BomPickerEntry> entries;  // parts that can be bought now: fitting ones first, then the usual order
  std::size_t leftOut = 0;              // found but not buyable now: out of stock, or priced out of proportion
  std::size_t otherValue = 0;           // buyable, but of another value than the line's while others fit
};
// The search answer ordered for the picker. A part that is out of stock or priced out of proportion (see
// orderableOffer) is never listed; it is counted in leftOut. A part stating another value than the line's
// is not listed when parts that fit are; it is counted in otherValue. At most `limit` entries.
BomPickerRanking rankBomPickerCandidates(const BomLine& line, std::uint64_t quantity,
                                         const std::vector<VendorCandidate>& candidates, std::size_t limit = 30);
// The picker's line under its title: the store, and the quantity the parts are for.
std::string bomPickerContext(const std::string& store, std::uint64_t quantity);
// The picker's line under the search once the lookup has answered: how many parts are listed and how many
// were left out. Plain words from one template, so the same answer always reads the same.
std::string bomPickerSummary(const std::string& store, std::size_t listed, std::size_t leftOut,
                             std::size_t otherValue = 0);
// Whether manual search can list parts for a store. All three stores can.
bool bomPickerSearchable(const std::string& store);
// A warning to show beside the store when its answers are less reliable. Empty for a store with no warning.
std::string bomPickerStoreNote(const std::string& store);

}  // namespace inventatory
