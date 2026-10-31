#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace freedesktop {

  enum class ParseFailure {
    NOT_FOUND,
    READ_ERROR,
    INVALID_ENCODING,
  };

  struct ParseError {
    ParseFailure reason;
    std::string message;
  };

  struct ParseResult;

  // A tolerant freedesktop key file. Syntax diagnostics are retained while valid
  // entries remain available; duplicate keys use the last value.
  class KeyFile {
  public:
    using Section = std::map<std::string, std::string, std::less<>>;

    [[nodiscard]] const Section* section(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> value(std::string_view section, std::string_view key) const;
    // Section names in the order they first appear in the file.
    [[nodiscard]] const std::vector<std::string>& sectionNames() const noexcept { return m_sectionOrder; }

  private:
    using Sections = std::map<std::string, Section, std::less<>>;

    Sections m_sections;
    std::vector<std::string> m_sectionOrder;

    friend std::expected<ParseResult, ParseError> parseKeyFileContents(std::string_view contents);
  };

  struct ParseDiagnostic {
    std::size_t line = 0;
    std::string message;
  };

  struct ParseResult {
    KeyFile file;
    std::vector<ParseDiagnostic> diagnostics;
  };

  // Fails only when the file cannot be read or is not valid UTF-8.
  [[nodiscard]] std::expected<ParseResult, ParseError> parseKeyFile(const std::filesystem::path& path);
  // Same, for key file text already in memory. Fails only on invalid UTF-8.
  [[nodiscard]] std::expected<ParseResult, ParseError> parseKeyFileContents(std::string_view contents);

  // Decodes the escapes allowed in string values (\s \n \t \r \\). Values are
  // stored raw; callers unescape the ones that may carry them.
  [[nodiscard]] std::string unescapeValue(std::string_view value);

} // namespace freedesktop
