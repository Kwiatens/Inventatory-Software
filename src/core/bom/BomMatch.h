// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"
#include "import/kicad/KicadBom.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

using std::map;
using std::optional;
using std::string;
using std::vector;

enum class ValueKind {
  None,
  Capacitance,
  Resistance,
  Inductance,
  Frequency,
};

// The footprint's own name without the KiCad library in front of it:
// "Capacitor_SMD:C_0603_1608Metric" -> "C_0603_1608Metric". A name with no library is returned as it is.
string footprintName(const string& footprint);

// Reduces a footprint to the package token used by distributor data:
// "C_0603_1608Metric" -> "0603", "SOIC-8_3.9x4.9mm_P1.27mm" -> "SOIC-8",
// "R0402" -> "0402", "CAPC1608X90N" -> "0603".
string packageFromFootprint(const string& footprint);

// True when two package names describe the same package. Chip packages must be the same chip.
bool packageMatches(const string& lhs, const string& rhs);

// Parses values such as "4.7uH", "10K" or "R280" (RKM: 0.28 ohm, unlike "280R") into SI base units.
optional<double> parseElectricalValue(const string& text, ValueKind& kind);

// True when a designation reads as a part number rather than a component value.
bool looksLikePartNumber(const string& designation);

// Key that identifies a BOM line across re-analysis.
string bomLineKey(const BomLine& line);

struct BomMatchCandidate {
  string itemId;
  int score = 0;
  // The line's part number matches an identifier field, or is a word of the name.
  // Only identity matches are accepted on their own.
  bool identity = false;
};

// Why a line with candidates needs the user to confirm it.
enum class BomCheckReason {
  None,                // matched automatically, or nothing in stock to choose from
  Ambiguous,           // several stock items fit the line and nothing identifies one of them
  IdentityNotInStock,  // the BOM names a part number that no stock item carries
  QuantityConflict,    // the Quantity column disagrees with the designators on the row
  Unverified,          // only part with the right value, but package unknown or different
  OverrideLost,        // a saved confirmation no longer applies: the item changed or is gone
};

struct BomMatch {
  size_t lineIndex = 0;
  vector<BomMatchCandidate> candidates;  // ranked, best first; empty means no stock
  size_t chosen = 0;
  int needed = 0;     // quantityPerBoard * boards
  int available = 0;  // chosen item's on-hand quantity
  // Authorised by the user or an automatic match. Only these are picked or deducted.
  bool confirmed = false;
  bool sufficient = false;  // confirmed, and on hand for total confirmed demand
  BomCheckReason check = BomCheckReason::None;  // why the line needs the user, when it does

  const BomMatchCandidate* chosenCandidate() const {
    return chosen < candidates.size() ? &candidates[chosen] : nullptr;
  }
  string chosenItemId() const {
    const auto* candidate = chosenCandidate();
    return candidate == nullptr ? string() : candidate->itemId;
  }
  bool needsConfirmation() const { return !confirmed && !candidates.empty(); }
};

struct BomAnalysis {
  int boards = 1;
  vector<BomLine> lines;
  vector<BomMatch> matches;  // parallel to lines
  int readyCount = 0;        // confirmed and on hand
  int shortCount = 0;        // nothing in stock, or confirmed but short
  int confirmCount = 0;      // stock candidates exist and the user has not confirmed one
  int totalPieces = 0;
};

// The stock item the user accepted for a line. Honoured only while its fingerprint still matches.
struct BomConfirmation {
  string itemId;
  string fingerprint;
};

// Confirmation value for a line the user said stock does not have: it is bought, as if no stock part
// matched it.
inline constexpr const char* kBomNotInStock = "@not-in-stock";

// Fingerprint of the item fields the matcher reads. 16 lowercase hex digits, stable across builds.
string bomItemFingerprint(const InventoryItem& item);
// Stored form, kept in a project's confirmation map: "<itemId>\x1e<fingerprint>".
string encodeBomConfirmation(const BomConfirmation& confirmation);
// Nullopt for incomplete values, including the bare item ids that older versions stored.
optional<BomConfirmation> decodeBomConfirmation(const string& value);

// Same part number, or same value and package: the rules that accept stock automatically.
bool bomSuggestionFitsLine(const BomLine& line, const InventoryItem& suggestion);

// Browsable while short, but completable only when every line has enough confirmed stock.
bool bomBuildReady(const BomAnalysis& analysis);

// Minimum score for a candidate to be listed. Listing never selects a part.
constexpr int kBomMatchThreshold = 60;

// `confirmations` maps a line key to an encoded BomConfirmation. A stale or undecodable value
// leaves the line unconfirmed and never picks another part. `suggestions` moves the chosen
// candidate for "next match" but never confirms it.
BomAnalysis analyzeBom(const KicadBomFile& bom, const vector<InventoryItem>& items, int boards,
                       const map<string, string>& confirmations, const map<string, string>& suggestions = {});

// Recomputes needed/available/sufficient and the status counts after a confirmation or a board
// count change, without rescoring every candidate.
void recomputeBomTotals(BomAnalysis& analysis, const vector<InventoryItem>& items);

}  // namespace inventatory
