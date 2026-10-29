// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Common.h"

#include <vector>
#include <string>

#include <stdarg.h>

namespace Luau
{

std::string format(const char* fmt, ...) LUAU_PRINTF_ATTR(1, 2);
std::string vformat(const char* fmt, va_list args);

void formatAppend(std::string& str, const char* fmt, ...) LUAU_PRINTF_ATTR(2, 3);
void vformatAppend(std::string& ret, const char* fmt, va_list args);

std::string join(const std::vector<std::string_view>& segments, std::string_view delimiter);
std::string join(const std::vector<std::string>& segments, std::string_view delimiter);

std::vector<std::string_view> split(std::string_view s, char delimiter);

// Computes the Damerau-Levenshtein distance of A and B.
// https://en.wikipedia.org/wiki/Damerau-Levenshtein_distance#Distance_with_adjacent_transpositions
size_t editDistance(std::string_view a, std::string_view b);

bool startsWith(std::string_view haystack, std::string_view needle);
bool equalsLower(std::string_view lhs, std::string_view rhs);

size_t hashRange(const char* data, size_t size);

std::string escape(std::string_view s, bool escapeForInterpString = false);
bool isIdentifier(std::string_view s);

std::string_view strip(std::string_view s);

} // namespace Luau
