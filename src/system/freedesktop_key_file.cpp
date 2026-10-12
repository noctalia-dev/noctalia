#include "system/freedesktop_key_file.h"

#include <cctype>
#include <fstream>
#include <string_view>

namespace {

  std::string_view trimView(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
      value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
      value.remove_suffix(1);
    }
    return value;
  }

  bool isContinuation(unsigned char value) { return (value & 0xC0U) == 0x80U; }

  bool validUtf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
      const auto first = static_cast<unsigned char>(value[i]);
      if (first <= 0x7FU) {
        ++i;
        continue;
      }

      std::size_t sequenceLength = 0;
      unsigned char secondMin = 0x80U;
      unsigned char secondMax = 0xBFU;
      if (first >= 0xC2U && first <= 0xDFU) {
        sequenceLength = 2;
      } else if (first == 0xE0U) {
        sequenceLength = 3;
        secondMin = 0xA0U;
      } else if (first >= 0xE1U && first <= 0xECU) {
        sequenceLength = 3;
      } else if (first == 0xEDU) {
        sequenceLength = 3;
        secondMax = 0x9FU;
      } else if (first >= 0xEEU && first <= 0xEFU) {
        sequenceLength = 3;
      } else if (first == 0xF0U) {
        sequenceLength = 4;
        secondMin = 0x90U;
      } else if (first >= 0xF1U && first <= 0xF3U) {
        sequenceLength = 4;
      } else if (first == 0xF4U) {
        sequenceLength = 4;
        secondMax = 0x8FU;
      } else {
        return false;
      }

      if (i + sequenceLength > value.size()) {
        return false;
      }
      const auto second = static_cast<unsigned char>(value[i + 1]);
      if (second < secondMin || second > secondMax) {
        return false;
      }
      for (std::size_t offset = 2; offset < sequenceLength; ++offset) {
        if (!isContinuation(static_cast<unsigned char>(value[i + offset]))) {
          return false;
        }
      }
      i += sequenceLength;
    }
    return true;
  }

  freedesktop::ParseError makeError(freedesktop::ParseFailure reason, std::string_view message) {
    return {
        .reason = reason,
        .message = std::string(message),
    };
  }

} // namespace

namespace freedesktop {

  const KeyFile::Section* KeyFile::section(std::string_view name) const {
    const auto it = m_sections.find(name);
    return it == m_sections.end() ? nullptr : &it->second;
  }

  std::optional<std::string_view> KeyFile::value(std::string_view sectionName, std::string_view key) const {
    const Section* values = section(sectionName);
    if (values == nullptr) {
      return std::nullopt;
    }
    const auto it = values->find(key);
    return it == values->end() ? std::nullopt : std::optional<std::string_view>(it->second);
  }

  std::expected<ParseResult, ParseError> parseKeyFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      std::error_code error;
      if (!std::filesystem::exists(path, error) && !error) {
        return std::unexpected(makeError(ParseFailure::NOT_FOUND, "file does not exist"));
      }
      return std::unexpected(makeError(ParseFailure::READ_ERROR, "cannot open file"));
    }

    ParseResult result;
    std::string currentSection;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(file, line)) {
      ++lineNumber;
      if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF")) {
        line.erase(0, 3);
      }
      if (!validUtf8(line)) {
        return std::unexpected(
            makeError(ParseFailure::INVALID_ENCODING, "invalid UTF-8 at line " + std::to_string(lineNumber))
        );
      }

      const std::string_view trimmed = trimView(line);
      if (trimmed.empty() || trimmed.front() == '#') {
        continue;
      }

      if (trimmed.front() == '[') {
        if (trimmed.size() < 3 || trimmed.back() != ']') {
          result.diagnostics.push_back({lineNumber, "malformed section header"});
          currentSection.clear();
          continue;
        }
        const std::string_view sectionName = trimView(trimmed.substr(1, trimmed.size() - 2));
        if (sectionName.empty()) {
          result.diagnostics.push_back({lineNumber, "empty section name"});
          currentSection.clear();
          continue;
        }
        const auto [it, inserted] = result.file.m_sections.emplace(std::string(sectionName), KeyFile::Section{});
        if (!inserted) {
          result.diagnostics.push_back({lineNumber, "duplicate section '" + std::string(sectionName) + "'"});
        }
        currentSection = it->first;
        continue;
      }

      const std::size_t equals = trimmed.find('=');
      if (equals == std::string_view::npos) {
        result.diagnostics.push_back({lineNumber, "malformed assignment"});
        continue;
      }
      if (currentSection.empty()) {
        result.diagnostics.push_back({lineNumber, "assignment outside a section"});
        continue;
      }

      const std::string key(trimView(trimmed.substr(0, equals)));
      if (key.empty()) {
        result.diagnostics.push_back({lineNumber, "empty key"});
        continue;
      }
      const std::string value(trimView(trimmed.substr(equals + 1)));
      auto& values = result.file.m_sections.at(currentSection);
      const auto [it, inserted] = values.insert_or_assign(key, value);
      if (!inserted) {
        result.diagnostics.push_back({lineNumber, "duplicate key '" + key + "' in section '" + currentSection + "'"});
      }
      (void)it;
    }

    if (file.bad()) {
      return std::unexpected(makeError(ParseFailure::READ_ERROR, "I/O error"));
    }
    return result;
  }

} // namespace freedesktop
