// Inventatory - Hardware Inventory Management System
// Delimiter-agnostic CSV scanning shared by every importer.

#pragma once

#include <initializer_list>
#include <string>
#include <vector>

namespace inventatory {

using std::initializer_list;
using std::string;
using std::vector;

// Removes a leading UTF-8 byte order mark so the first header cell stays clean.
string stripByteOrderMark(string text);

// Picks the delimiter used by the first non-blank line: ',', ';' or '\t'.
// Quoted sections are ignored so a quoted "2,62800 zl" cannot win the vote.
char sniffDelimiter(const string& text);

// RFC4180-ish scanner. Doubled quotes inside a quoted field yield one quote,
// CR is dropped everywhere, and fully blank rows are discarded.
vector<vector<string>> parseCsv(const string& text, char delimiter, string& error);

// Lowercases and strips every non-alphanumeric character, so "Digi-Key Part #"
// and "digikey part number" collapse onto comparable keys.
string normalizeHeader(string value);

bool anyHeaderMatches(const string& header, initializer_list<const char*> aliases);

// Index of the first header cell matching any alias, or -1.
int findColumn(const vector<string>& headers, initializer_list<const char*> aliases);

// Bounds-safe trimmed cell access; empty string for a negative index.
string csvCell(const vector<string>& row, int index);

}  // namespace inventatory
