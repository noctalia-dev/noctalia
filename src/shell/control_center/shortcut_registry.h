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

  // Control Center keeps shortcut instances across panel open/close. Plugin-backed
  // shortcuts use these hooks to pause timers and source file watches while closed.
  virtual void onPanelClose() {}
  virtual void onPanelOpen() {}

  [[nodiscard]] std::string_view currentIcon() const { return active() ? iconOn() : iconOff(); }
};

struct ShortcutServices;
struct Config;

class ShortcutRegistry {
public:
  struct CatalogEntry {
    std::string_view type;
    std::string_view labelKey;
    bool literalLabel = false; // when true, labelKey holds a literal display name, not an i18n key
  };

  [[nodiscard]] static std::span<const CatalogEntry> catalog();
  // Whether a built-in shortcut's backing feature is enabled. Drives both the
  // Settings GUI add-list and create(); disabled features cannot be added.
  [[nodiscard]] static bool isAvailable(std::string_view type, const Config& config);
  static std::unique_ptr<Shortcut> create(std::string_view type, const ShortcutServices& services);
};
