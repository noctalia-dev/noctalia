#pragma once

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  // Where a config key was defined. An empty `file` means the position is
  // unknown, e.g. a key no source file sets or a value synthesized in memory.
  struct SourceOrigin {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    [[nodiscard]] bool valid() const { return !file.empty(); }

    // "file:line:column", or empty when unknown.
    [[nodiscard]] std::string format() const {
      if (file.empty()) {
        return {};
      }
      return file + ':' + std::to_string(line) + ':' + std::to_string(column);
    }

    // "file:line:column: text", or just `text` when unknown.
    [[nodiscard]] std::string prefixed(std::string_view text) const {
      if (file.empty()) {
        return std::string(text);
      }
      return format() + ": " + std::string(text);
    }

    // format(), shortened for narrow surfaces: the file path relative to
    // `baseDir` when it lives there, otherwise its file name.
    [[nodiscard]] std::string shortFormat(std::string_view baseDir) const {
      if (file.empty()) {
        return {};
      }
      std::string_view shortFile(file);
      if (!baseDir.empty()
          && shortFile.starts_with(baseDir)
          && shortFile.size() > baseDir.size()
          && shortFile[baseDir.size()] == '/') {
        shortFile.remove_prefix(baseDir.size() + 1);
      } else if (const std::size_t slash = shortFile.rfind('/'); slash != std::string_view::npos) {
        shortFile.remove_prefix(slash + 1);
      }
      return std::string(shortFile) + ':' + std::to_string(line) + ':' + std::to_string(column);
    }

    // prefixed(), using the shortened location.
    [[nodiscard]] std::string prefixedShort(std::string_view baseDir, std::string_view text) const {
      if (file.empty()) {
        return std::string(text);
      }
      return shortFormat(baseDir) + ": " + std::string(text);
    }
  };

  // Accumulates issues found while reading or validating a config table. The
  // same sink feeds the reload pipeline (warnings) and `noctalia config validate`
  // (errors). `path` is the dotted key path, e.g. "shell.animation.style".
  // `origin` is filled in afterwards by ConfigOriginIndex::annotate, except for
  // syntax errors that carry it from the start.
  struct Diagnostics {
    enum class Severity { Warning, Error };
    enum class RecoveryScope { Advisory, Value, Component, Document };

    struct Entry {
      Severity severity;
      RecoveryScope recoveryScope;
      std::string code;
      std::string path;
      std::string message;
      std::string ownerPath;
      SourceOrigin origin;

      // "file:line:column: path: message", dropping the position when unknown.
      [[nodiscard]] std::string describe() const { return origin.prefixed(path + ": " + message); }

      // describe(), shortened for the on-screen config-error notification.
      [[nodiscard]] std::string describeShort(std::string_view baseDir) const {
        return origin.prefixedShort(baseDir, path + ": " + message);
      }
    };

    std::vector<Entry> entries;

    void warn(std::string path, std::string message, std::string code = "config.warning") {
      entries.push_back(
          {Severity::Warning, RecoveryScope::Advisory, std::move(code), std::move(path), std::move(message), {}, {}}
      );
    }
    void error(std::string path, std::string message, std::string code = "config.invalid-value") {
      entries.push_back(
          {Severity::Error, RecoveryScope::Value, std::move(code), std::move(path), std::move(message), {}, {}}
      );
    }
    void componentError(
        std::string path, std::string ownerPath, std::string message, std::string code = "config.invalid-component"
    ) {
      entries.push_back(
          {Severity::Error,
           RecoveryScope::Component,
           std::move(code),
           std::move(path),
           std::move(message),
           std::move(ownerPath),
           {}}
      );
    }
    void fatal(std::string path, std::string message, std::string code = "config.invalid-document") {
      entries.push_back(
          {Severity::Error, RecoveryScope::Document, std::move(code), std::move(path), std::move(message), {}, {}}
      );
    }
    // Fatal error whose position is known up front (a parse failure, or the
    // include entry that named a missing file).
    void fatalAt(SourceOrigin origin, std::string path, std::string message, std::string code) {
      entries.push_back(
          {Severity::Error,
           RecoveryScope::Document,
           std::move(code),
           std::move(path),
           std::move(message),
           {},
           std::move(origin)}
      );
    }

    [[nodiscard]] bool hasErrors() const {
      for (const auto& e : entries) {
        if (e.severity == Severity::Error) {
          return true;
        }
      }
      return false;
    }

    [[nodiscard]] bool hasFatalErrors() const {
      for (const auto& e : entries) {
        if (e.severity == Severity::Error && e.recoveryScope == RecoveryScope::Document) {
          return true;
        }
      }
      return false;
    }

    // Origin is deliberately not compared: the same error moving to another line
    // is not a new error.
    [[nodiscard]] Diagnostics introducedErrorsComparedTo(const Diagnostics& baseline) const {
      Diagnostics introduced;
      for (const auto& candidate : entries) {
        if (candidate.severity != Severity::Error) {
          continue;
        }
        const bool existed = std::ranges::any_of(baseline.entries, [&](const Entry& previous) {
          return candidate.severity == previous.severity
              && candidate.recoveryScope == previous.recoveryScope
              && candidate.code == previous.code
              && candidate.path == previous.path
              && candidate.message == previous.message
              && candidate.ownerPath == previous.ownerPath;
        });
        if (!existed) {
          introduced.entries.push_back(candidate);
        }
      }
      return introduced;
    }
  };

  // Joins a parent path and a key into a dotted path, skipping the leading dot
  // when the parent is empty (top level).
  inline std::string joinPath(std::string_view parent, std::string_view key) {
    if (parent.empty()) {
      return std::string(key);
    }
    std::string out;
    out.reserve(parent.size() + 1 + key.size());
    out.append(parent);
    out.push_back('.');
    out.append(key);
    return out;
  }

} // namespace noctalia::config::schema
