#pragma once

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  // Accumulates issues found while reading or validating a config table. The
  // same sink feeds the reload pipeline (warnings) and `noctalia config validate`
  // (errors). `path` is the dotted key path, e.g. "shell.animation.style".
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
    };

    std::vector<Entry> entries;

    void warn(std::string path, std::string message, std::string code = "config.warning") {
      entries.push_back(
          {Severity::Warning, RecoveryScope::Advisory, std::move(code), std::move(path), std::move(message), {}}
      );
    }
    void error(std::string path, std::string message, std::string code = "config.invalid-value") {
      entries.push_back(
          {Severity::Error, RecoveryScope::Value, std::move(code), std::move(path), std::move(message), {}}
      );
    }
    void componentError(
        std::string path, std::string ownerPath, std::string message, std::string code = "config.invalid-component"
    ) {
      entries.push_back(
          {Severity::Error, RecoveryScope::Component, std::move(code), std::move(path), std::move(message),
           std::move(ownerPath)}
      );
    }
    void fatal(std::string path, std::string message, std::string code = "config.invalid-document") {
      entries.push_back(
          {Severity::Error, RecoveryScope::Document, std::move(code), std::move(path), std::move(message), {}}
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
