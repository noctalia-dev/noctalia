#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

class Shortcut {
public:
  virtual ~Shortcut() = default;

  [[nodiscard]] virtual std::string_view id() const = 0;
  [[nodiscard]] virtual std::string defaultLabel() const = 0;
  [[nodiscard]] virtual std::string displayLabel() const { return defaultLabel(); }
  [[nodiscard]] virtual std::string displayIcon() const { return std::string(currentIcon()); }
  [[nodiscard]] virtual std::string_view iconOn() const = 0;
  [[nodiscard]] virtual std::string_view iconOff() const = 0;
  [[nodiscard]] virtual bool isToggle() const { return false; }
  [[nodiscard]] virtual bool enabled() const { return true; }

  [[nodiscard]] virtual bool active() const { return false; }

  virtual void onClick() {}
  virtual void onRightClick() {}
  /// direction >= 0 scrolls forward (e.g. toward performance), direction < 0 backward.
  virtual void onScroll(int /*direction*/) {}

  [[nodiscard]] std::string_view currentIcon() const { return active() ? iconOn() : iconOff(); }
};

struct ShortcutServices;

class ShortcutRegistry {
public:
  struct CatalogEntry {
    std::string_view type;
    std::string_view labelKey;
    bool literalLabel = false; // when true, labelKey holds a literal display name, not an i18n key
  };

  [[nodiscard]] static std::span<const CatalogEntry> catalog();
  static std::unique_ptr<Shortcut> create(std::string_view type, const ShortcutServices& services);
};
