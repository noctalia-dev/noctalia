#include "config/state_store.h"

#include "config/atomic_file.h"
#include "core/log.h"

#include <format>
#include <sstream>
#include <utility>

namespace {
  constexpr Logger kLog("state");

  bool validStateIdentifier(std::string_view value) {
    if (value.empty()) {
      return false;
    }
    for (char c : value) {
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
        continue;
      }
      return false;
    }
    return true;
  }

  toml::table* ensureTable(toml::table& parent, std::string_view key) {
    if (auto* existing = parent.get_as<toml::table>(key)) {
      return existing;
    }
    if (parent.contains(key)) {
      kLog.warn("state owner {} is not a table; replacing it", key);
    }
    auto [it, _] = parent.insert_or_assign(key, toml::table{});
    return it->second.as_table();
  }

  std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }
} // namespace

StateStore::StateStore(std::filesystem::path path) : m_path(std::move(path)) {}

void StateStore::setPath(std::filesystem::path path) { m_path = std::move(path); }

void StateStore::load() {
  m_state = toml::table{};
  m_parseError.clear();

  if (m_path.empty() || !std::filesystem::exists(m_path)) {
    return;
  }

  kLog.info("loading {}", m_path.string());
  try {
    m_state = toml::parse_file(m_path.string());
  } catch (const toml::parse_error& e) {
    const auto& src = e.source();
    m_parseError = std::format(
        "{} line {}, column {}: {}", m_path.filename().string(), src.begin.line, src.begin.column, e.description()
    );
    kLog.warn(
        "parse error in {} at line {}, column {}: {}", m_path.string(), src.begin.line, src.begin.column,
        e.description()
    );
    m_state = toml::table{};
  }
}

std::optional<bool> StateStore::boolValue(std::string_view owner, std::string_view key) const {
  if (!validStateIdentifier(owner) || !validStateIdentifier(key)) {
    kLog.warn("invalid state key {}.{}", owner, key);
    return std::nullopt;
  }

  const auto* table = m_state[owner].as_table();
  if (table == nullptr) {
    if (m_state.contains(owner)) {
      kLog.warn("state owner {} is not a table", owner);
    }
    return std::nullopt;
  }

  const auto* node = table->get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (auto value = node->value<bool>()) {
    return value;
  }

  kLog.warn("state value {}.{} is not a bool", owner, key);
  return std::nullopt;
}

std::optional<std::string> StateStore::stringValue(std::string_view owner, std::string_view key) const {
  if (!validStateIdentifier(owner) || !validStateIdentifier(key)) {
    kLog.warn("invalid state key {}.{}", owner, key);
    return std::nullopt;
  }

  const auto* table = m_state[owner].as_table();
  if (table == nullptr) {
    if (m_state.contains(owner)) {
      kLog.warn("state owner {} is not a table", owner);
    }
    return std::nullopt;
  }

  const auto* node = table->get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (auto value = node->value<std::string>()) {
    return value;
  }

  kLog.warn("state value {}.{} is not a string", owner, key);
  return std::nullopt;
}

bool StateStore::setBool(std::string_view owner, std::string_view key, bool value) {
  if (m_path.empty()) {
    return false;
  }
  if (!validStateIdentifier(owner) || !validStateIdentifier(key)) {
    kLog.warn("invalid state key {}.{}", owner, key);
    return false;
  }

  auto* table = ensureTable(m_state, owner);
  if (table == nullptr) {
    return false;
  }

  if (auto existing = (*table)[key].value<bool>(); existing.has_value() && *existing == value) {
    return true;
  }

  table->insert_or_assign(key, value);
  if (!write()) {
    kLog.warn("failed to write {}", m_path.string());
    return false;
  }

  m_parseError.clear();
  return true;
}

bool StateStore::setString(std::string_view owner, std::string_view key, std::string_view value) {
  if (m_path.empty()) {
    return false;
  }
  if (!validStateIdentifier(owner) || !validStateIdentifier(key)) {
    kLog.warn("invalid state key {}.{}", owner, key);
    return false;
  }

  auto* table = ensureTable(m_state, owner);
  if (table == nullptr) {
    return false;
  }

  if (auto existing = (*table)[key].value<std::string>(); existing.has_value() && *existing == value) {
    return true;
  }

  table->insert_or_assign(key, std::string(value));
  if (!write()) {
    kLog.warn("failed to write {}", m_path.string());
    return false;
  }

  m_parseError.clear();
  return true;
}

bool StateStore::write() {
  if (m_path.empty()) {
    return false;
  }

  return writeTextFileAtomic(m_path, formatToml(m_state));
}
