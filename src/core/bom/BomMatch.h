// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Matching BOM lines against inventory by part number, value and package.

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

// Reduces a footprint to the package token used by distributor data:
// "C_0603_1608Metric" -> "0603", "SOIC-8_3.9x4.9mm_P1.27mm" -> "SOIC-8", "R0402" -> "0402", "CAPC1608X90N" -> "0603".
string packageFromFootprint(const string& footprint);

// True when two package descriptions refer to the same physical package. Chip packages must be the same chip.
bool packageMatches(const string& lhs, const string& rhs);

// Parses "1uF", "100nF", "10K", "280R", "R280" (RKM), "4.7uH" and "32.768KHz"
// into SI base units. Leading-R notation follows the RKM standard, so "R280"
// is 0.28 ohm while "280R" is 280 ohm.
optional<double> parseElectricalValue(const string& text, ValueKind& kind);

// True when a designation reads as a part number rather than a component value.
bool looksLikePartNumber(const string& designation);

// Stable key identifying a BOM line across re-analysis, used for confirmations and supplier suggestions.
string bomLineKey(const BomLine& line);

struct BomMatchCandidate {
  string itemId;
  int score = 0;
  // The stock item carries the line's part number: its SKU, DigiKey number or manufacturer part number equals the
  // line's, or the part number is a whole word of its name. Only identity matches can be accepted on their own.
  bool identity = false;
};

// Why a line that has candidates is not matched without the user's confirmation.
enum class BomCheckReason {
  None,                // matched automatically, or nothing in stock to choose from
  Ambiguous,           // several stock items fit the line and nothing identifies one of them
  IdentityNotInStock,  // the BOM names a part number that no stock item carries
  QuantityConflict,    // the Quantity column disagrees with the designators on the row
  Unverified,          // the only stock part has the right value, but its package is unknown or differs
  OverrideLost,        // a saved confirmation no longer applies: the item changed or is gone
};

struct BomMatch {
  size_t lineIndex = 0;
  vector<BomMatchCandidate> candidates;  // ranked, best first; empty means no stock
  size_t chosen = 0;
  int needed = 0;     // quantityPerBoard * boards
  int available = 0;  // chosen item's on-hand quantity
  // The chosen part is authorised: confirmed by the user, or matched automatically. Only an authorised match is
  // ever picked from or deducted. Anything else is listed as a candidate and waits for the user.
  bool confirmed = false;
  bool sufficient = false;  // confirmed and on hand for the total confirmed demand on the chosen part
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

// A confirmation records which stock item the user accepted for a BOM line, and that item's fingerprint at the
// time. It is honoured only while the item still has the same fingerprint.
struct BomConfirmation {
  string itemId;
  string fingerprint;
};

// Fingerprint of the item fields the matcher reads. 16 lowercase hex digits, stable across builds.
string bomItemFingerprint(const InventoryItem& item);
// Stored form, kept in a project's confirmation map: "<itemId>\x1e<fingerprint>".
string encodeBomConfirmation(const BomConfirmation& confirmation);
// Nullopt for anything that is not a complete confirmation, including the bare item ids that older versions
// stored. Those lines are unconfirmed until the user confirms them again.
optional<BomConfirmation> decodeBomConfirmation(const string& value);

// True when a supplier's part fits the BOM line by the same rules that accept stock automatically: the same part
// number, or the same value and package. Used to decide whether a supplier suggestion is shown as verified.
bool bomSuggestionFitsLine(const BomLine& line, const InventoryItem& suggestion);

// A build may be browsed while short, but it can only be completed when every BOM line has a confirmed
// stock item with enough units for all requested boards.
bool bomBuildReady(const BomAnalysis& analysis);

// Score at or above which a candidate is listed for the user to choose from. Listing alone never selects a part.
constexpr int kBomMatchThreshold = 60;

// `confirmations` maps bomLineKey to an encoded BomConfirmation. A value that does not decode, or whose
// fingerprint no longer matches, leaves the line unconfirmed rather than choosing another part.
// `suggestions` maps bomLineKey to the item the user is looking at with "next match". It moves the chosen
// candidate but never confirms it.
BomAnalysis analyzeBom(const KicadBomFile& bom, const vector<InventoryItem>& items, int boards,
                       const map<string, string>& confirmations, const map<string, string>& suggestions = {});

// Recomputes needed/available/sufficient and the status counts after a confirmation or a board
// count change, without rescoring every candidate.
void recomputeBomTotals(BomAnalysis& analysis, const vector<InventoryItem>& items);

}  // namespace inventatory
