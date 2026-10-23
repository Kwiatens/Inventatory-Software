// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"

#include <string>
#include <vector>

namespace inventatory {

// The DigiKey top-level family of a TME category, so the same rules classify both vendors' parts.
// Empty when no rule matches; the caller keeps the TME category name.
std::string tmeCategoryFamily(const std::string& categoryName);

// TME's description in the list form DigiKey names use: "Connector: pin strips; socket; PIN: 40; THT" becomes
// "CONNECTOR PIN STRIPS SOCKET PIN 40 THT". Colons after a label and the semicolons between attributes are
// dropped, letters are upper-cased, and the first number stays where the Stock list highlights it. A
// description with nothing to rewrite is returned trimmed.
std::string tmeListName(const std::string& description);

// The name a part gets in Stock. A connector with a known pin count reads as a short phrase, "Female 40 Pin
// Connector 2.54mm", using the gender in the description and the pitch among the parameters. Every other
// description falls back to tmeListName.
std::string tmeReadableName(const std::string& description, const std::vector<Parameter>& parameters);

// The DigiKey name for the same quantity, or TME's name when there is none. The family selects the
// meaning for names that depend on the kind of part.
std::string tmeParameterName(const std::string& name, const std::string& family);

// The value in DigiKey's form: "-40 to 85°C" not "-40...85°C", "16V" not "16V DC".
// Other values are returned as sent.
std::string tmeParameterValue(const std::string& value);

// The DigiKey parameters for one TME parameter, usually just one. "Type of transistor" becomes
// "FET Type" or "Transistor Type"; "Type of diode" also sets "Technology".
std::vector<Parameter> tmeParameters(const std::string& name, const std::string& value, const std::string& family);

}  // namespace inventatory
