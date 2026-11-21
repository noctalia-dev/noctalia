#pragma once

#include "config/widget_setting_value.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

enum class GreeterAnchor : std::uint8_t {
  TopLeft,
  TopCenter,
  TopRight,
  CenterLeft,
  Center,
  CenterRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};
enum class GreeterLoginMode : std::uint8_t { Auto, Full, Minimal };

struct GreeterCoreOverride {
  std::optional<bool> visible;
  std::optional<GreeterAnchor> anchor;
  std::optional<double> x;
  std::optional<double> y;
  std::optional<double> width;
  std::optional<std::string> output;
  std::optional<double> rotation;
  std::optional<bool> flipX;
  std::optional<bool> flipY;
  std::optional<double> scale;
  bool operator==(const GreeterCoreOverride&) const = default;
};

struct GreeterLayoutConfig {
  std::optional<GreeterLoginMode> loginMode;
  std::map<std::string, GreeterCoreOverride> components;
  bool operator==(const GreeterLayoutConfig&) const = default;
};

struct GreeterAnchorPlacement {
  GreeterAnchor anchor = GreeterAnchor::Center;
  double x = 0;
  double y = 0;
  bool operator==(const GreeterAnchorPlacement&) const = default;
};

struct GreeterRelativePlacement {
  double cx = 0;
  double cy = 0;
  double placementWidth = 1920;
  double placementHeight = 1080;
  bool operator==(const GreeterRelativePlacement&) const = default;
};

struct GreeterWidgetConfig {
  std::string id;
  std::string type;
  bool enabled = true;
  std::variant<GreeterAnchorPlacement, GreeterRelativePlacement> placement;
  std::optional<std::string> output;
  double boxWidth = 0;
  double boxHeight = 0;
  double rotation = 0;
  bool flipX = false;
  bool flipY = false;
  std::unordered_map<std::string, WidgetSettingValue> settings;
  bool operator==(const GreeterWidgetConfig&) const = default;
};

struct GreeterWidgetCollection {
  // Unset uses the default collection; an engaged vector, even empty, is custom, back to front.
  std::optional<std::vector<GreeterWidgetConfig>> instances;
  bool operator==(const GreeterWidgetCollection&) const = default;
};

struct ShellGreeterConfig {
  std::optional<GreeterLayoutConfig> layout;
  // Absent leaves the installed collection unmanaged; an empty table explicitly requests defaults.
  std::optional<GreeterWidgetCollection> widgets;
  bool operator==(const ShellGreeterConfig&) const = default;
};
