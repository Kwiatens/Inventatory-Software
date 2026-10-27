// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Mouser's search answer carries packaging and pack size as its only attributes. The value, tolerance, rating and
// size of a passive part stand in its description alone ("Thick Film Resistors - SMD 10K OHM 1%", "Multilayer
// Ceramic Capacitors MLCC - SMD/SMT 50V 0.1uF X7R 0603 10%"). This reads them into DigiKey's parameter labels, so
// the BOM matcher checks Mouser parts by the same rules as DigiKey's.
//
// Only what the text states plainly is read. A quantity written twice with different values, or a category the
// reader does not know, gives no parameter, and the part then stays under the "might not match" heading. Electrolytic capacitors
// and connectors are not read: their descriptions do not name the can or the contacts reliably.

#pragma once

#include "core/inventory/Inventory.h"

#include <string>
#include <vector>

namespace inventatory {

// DigiKey-labelled parameters (Resistance, Capacitance, Inductance, Frequency, Current Rating (Amps), Tolerance,
// Voltage - Rated, Temperature Coefficient, Package / Case, Size / Dimension, Color) found in the description of a
// part of this Mouser category. Empty for any other category.
std::vector<Parameter> mouserDescriptionParameters(const std::string& category, const std::string& description);

}  // namespace inventatory
