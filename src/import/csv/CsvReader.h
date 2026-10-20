// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Delimiter-agnostic CSV scanning shared by every importer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace inventatory {

using std::initializer_list;
using std::size_t;
using std::string;
using std::vector;

// The largest CSV text, in bytes, any importer reads.
inline constexpr std::uintmax_t kMaximumCsvInputBytes = 25U * 1024U * 1024U;

inline constexpr size_t kMaximumCsvCells = 2000000;

// Removes a leading UTF-8 byte order mark so the first header cell stays clean.
string stripByteOrderMark(string text);

// True when `text` is well-formed UTF-8 (no overlong forms, surrogates or code points above U+10FFFF).
bool isValidUtf8(const string& text);

// Converts CSV bytes to UTF-8, dropping a UTF-8 BOM and converting UTF-16 (BOM FF FE or FE FF).
// Other input must be valid UTF-8; legacy code pages are refused rather than guessed.
bool decodeCsvBytes(const string& bytes, string& utf8, string& error);

// Reads and decodes a CSV file up to kMaximumCsvInputBytes; `kind` names it in messages.
bool readCsvFile(const std::filesystem::path& path, const string& kind, string& utf8, string& error);

// Picks ',', ';' or '\t' from the first non-blank line. Quoted sections are ignored.
char sniffDelimiter(const string& text);

// Doubled quotes yield one quote, CR is dropped, and line breaks in quoted fields become spaces.
// Blank rows are discarded. Scanning stops after `maxRows` non-blank rows.
vector<vector<string>> parseCsv(const string& text, char delimiter, string& error,
                                size_t maxRows = (std::numeric_limits<size_t>::max)());

// Like parseCsv, but \" inside quotes is one quote (Altium 365 writes inch marks this way).
vector<vector<string>> parseCsvBackslashQuotes(const string& text, char delimiter, string& error);

// Lowercases and drops non-alphanumerics, so "Digi-Key Part #" matches "digikey part number".
string normalizeHeader(string value);

bool anyHeaderMatches(const string& header, initializer_list<const char*> aliases);

// Index of the first header cell matching any alias, or -1.
int findColumn(const vector<string>& headers, initializer_list<const char*> aliases);

// Trimmed cell; empty when the index is negative or out of range.
string csvCell(const vector<string>& row, int index);

}  // namespace inventatory
