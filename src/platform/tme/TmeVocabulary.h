// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"

#include <string>
#include <vector>

namespace inventatory {

// The top-level category, in DigiKey's terms, that a TME category belongs to: "SMD resistors" is "Resistors", and
// "SMD N channel transistors" is "Discrete Semiconductor Products". Label, rack and detail rules are written in these
// terms, so a TME part is classified as the same kind of part a DigiKey part is. Empty when no rule matches; the
// caller keeps the TME category name then.
std::string tmeCategoryFamily(const std::string& categoryName);

// The name a TME parameter is stored under: the name the label, rack and detail rules read, which is DigiKey's name
// for the same quantity. Names with no counterpart are returned as TME sent them. The family is the top-level family
// from tmeCategoryFamily, for the names whose meaning depends on the kind of part.
std::string tmeParameterName(const std::string& name, const std::string& family);

// A TME parameter value written the way DigiKey writes it: a range reads "-40 to 85°C" rather than "-40...85°C", and
// a voltage has no direct-current qualifier ("16V", not "16V DC"). Other values are returned as sent.
std::string tmeParameterValue(const std::string& value);

// The parameters one TME parameter stands for, under the names and values the app reads. Most become one parameter;
// a TME parameter whose value names a kind of part becomes the DigiKey parameter for that kind: "Type of transistor"
// is "FET Type" for a MOSFET and "Transistor Type" for a bipolar transistor, and "Type of diode" also says the
// diode's technology (a Schottky diode is "Technology: Schottky", as DigiKey writes it).
std::vector<Parameter> tmeParameters(const std::string& name, const std::string& value, const std::string& family);

}  // namespace inventatory
