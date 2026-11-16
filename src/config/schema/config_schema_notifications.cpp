#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"
#include "notification/notification_filter.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  namespace {
    const Schema<OsdKindsConfig>& osdKindsSchema() {
      static const Schema<OsdKindsConfig> s = {
          field(&OsdKindsConfig::volume, "volume"),
          field(&OsdKindsConfig::volumeOutput, "volume_output"),
          field(&OsdKindsConfig::volumeInput, "volume_input"),
          field(&OsdKindsConfig::brightness, "brightness"),
          field(&OsdKindsConfig::wifi, "wifi"),
          field(&OsdKindsConfig::bluetooth, "bluetooth"),
          field(&OsdKindsConfig::powerProfile, "power_profile"),
          field(&OsdKindsConfig::caffeine, "caffeine"),
          field(&OsdKindsConfig::nightlight, "nightlight"),
          field(&OsdKindsConfig::dnd, "dnd"),
          field(&OsdKindsConfig::lockKeys, "lock_keys"),
          field(&OsdKindsConfig::keyboardLayout, "keyboard_layout"),
          field(&OsdKindsConfig::media, "media"),
          field(&OsdKindsConfig::mediaVolume, "media_volume"),
          field(&OsdKindsConfig::privacy, "privacy"),
          field(&OsdKindsConfig::keyboardBacklight, "keyboard_backlight"),
          field(&OsdKindsConfig::custom, "custom"),
      };
      return s;
    }
  } // namespace

  const Schema<OsdConfig>& osdSchema() {
    static const Schema<OsdConfig> s = {
        field(&OsdConfig::enabled, "enabled"),
        field(&OsdConfig::position, "position"),
        field(&OsdConfig::positionVertical, "position_vertical"),
        field(&OsdConfig::orientation, "orientation"),
        field(&OsdConfig::hideDelayMs, "hide_delay_ms", kOsdHideDelayMsRange),
        field(&OsdConfig::scale, "scale", kScaleRange),
        field(&OsdConfig::backgroundOpacity, "background_opacity", kUnitRange),
        field(&OsdConfig::border, "border"),
        colorField(&OsdConfig::borderColor, "border_color"),
        field(&OsdConfig::borderWidth, "border_width", kOsdBorderWidthRange),
        field(&OsdConfig::followFocusedOutput, "follow_focused_output"),
        field(&OsdConfig::offsetX, "offset_x", Range<std::int64_t>{0, std::nullopt}),
        field(&OsdConfig::offsetY, "offset_y", Range<std::int64_t>{0, std::nullopt}),
        field(&OsdConfig::monitors, "monitors"),
        subTable(&OsdConfig::kinds, "kinds", osdKindsSchema()),
    };
    return s;
  }

  namespace {
    const Schema<NotificationFilterConfig>& notificationFilterSchema() {
      static const Schema<NotificationFilterConfig> s = {
          field(&NotificationFilterConfig::enabled, "enabled"),
          custom<NotificationFilterConfig>(
              "matches",
              [](const toml::table& tbl, NotificationFilterConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (!out.match.empty()) {
                  return;
                }
                const auto* arr = tbl["matches"].as_array();
                if (arr == nullptr) {
                  warnWrongType(tbl, "matches", parentPath, diag, "an array of strings");
                  return;
                }
                for (const auto& token : readStringArray(*arr, joinPath(parentPath, "matches"), diag)) {
                  if (StringUtils::trim(token).empty()) {
                    continue;
                  }
                  out.match = normalizeNotificationMatchToken(token);
                  return;
                }
              },
              [](toml::table&, const NotificationFilterConfig&) {}
          ),
          field(&NotificationFilterConfig::match, "match"),
          field(&NotificationFilterConfig::matchContent, "match_content"),
          field(&NotificationFilterConfig::showToast, "show_toast"),
          field(&NotificationFilterConfig::saveHistory, "save_history"),
          field(&NotificationFilterConfig::playSound, "play_sound"),
          field(&NotificationFilterConfig::bypassDnd, "bypass_dnd"),
          field(&NotificationFilterConfig::allowPermanent, "allow_permanent"),
          field(&NotificationFilterConfig::overrideDuration, "override_duration"),
          field(&NotificationFilterConfig::allowedUrgencies, "allowed_urgencies"),
          custom<NotificationFilterConfig>(
              "allow_critical", [](const toml::table&, NotificationFilterConfig&, std::string_view, Diagnostics&) {},
              [](toml::table&, const NotificationFilterConfig&) {}
          ),
          finalize<NotificationFilterConfig>([](NotificationFilterConfig& filter, std::string_view, Diagnostics&) {
            filter.match = normalizeNotificationMatchToken(filter.match);
            filter.allowedUrgencies = normalizeFilterAllowedUrgencyStrings(filter.allowedUrgencies);
          }),
      };
      return s;
    }
  } // namespace

  const Schema<NotificationConfig>& notificationSchema() {
    static const Schema<NotificationConfig> s = {
        field(&NotificationConfig::enableDaemon, "enable_daemon"),
        field(&NotificationConfig::showAppName, "show_app_name"),
        field(&NotificationConfig::colorizeIcons, "colorize_icons"),
        field(&NotificationConfig::showActions, "show_actions"),
        field(&NotificationConfig::showProgressBar, "show_progress_bar"),
        field(&NotificationConfig::groupByApp, "group_by_app"),
        field(&NotificationConfig::position, "position"),
        field(&NotificationConfig::layer, "layer"),
        field(&NotificationConfig::scale, "scale", kScaleRange),
        field(&NotificationConfig::width, "width", kNotificationWidthRange),
        field(&NotificationConfig::backgroundOpacity, "background_opacity", kUnitRange),
        field(&NotificationConfig::border, "border"),
        colorField(&NotificationConfig::borderColor, "border_color"),
        field(&NotificationConfig::borderWidth, "border_width", kNotificationToastBorderWidthRange),
        field(&NotificationConfig::followFocusedOutput, "follow_focused_output"),
        field(&NotificationConfig::offsetX, "offset_x"),
        field(&NotificationConfig::offsetY, "offset_y"),
        field(&NotificationConfig::monitors, "monitors"),
        field(&NotificationConfig::newestOnEdge, "newest_on_edge"),
        field(&NotificationConfig::collapseOnDismiss, "collapse_on_dismiss"),
        field(&NotificationConfig::keepDismissedInHistory, "keep_dismissed_in_history"),
        field(&NotificationConfig::historyRetentionHours, "history_retention_hours", Range<std::int64_t>{0, 8760}),
        field(&NotificationConfig::maxVisible, "max_visible", Range<std::int64_t>{0, 20}),
        custom<NotificationConfig>(
            "blacklist",
            [](const toml::table& tbl, NotificationConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (!out.filters.empty()) {
                return;
              }
              const auto* arr = tbl["blacklist"].as_array();
              if (arr == nullptr) {
                warnWrongType(tbl, "blacklist", parentPath, diag, "an array of strings");
                return;
              }
              for (const auto& token : readStringArray(*arr, joinPath(parentPath, "blacklist"), diag)) {
                if (StringUtils::trim(token).empty()) {
                  continue;
                }
                NotificationFilterConfig filter;
                filter.match = normalizeNotificationMatchToken(token);
                filter.showToast = false;
                filter.saveHistory = false;
                filter.playSound = false;
                out.filters.push_back(std::move(filter));
              }
              normalizeNotificationFilterNames(out.filters);
            },
            [](toml::table&, const NotificationConfig&) {}
        ),
        custom<NotificationConfig>(
            "blacklist_allow_critical", [](const toml::table&, NotificationConfig&, std::string_view, Diagnostics&) {},
            [](toml::table&, const NotificationConfig&) {}
        ),
        custom<NotificationConfig>(
            "filter_order", [](const toml::table&, NotificationConfig&, std::string_view, Diagnostics&) {},
            [](toml::table& tbl, const NotificationConfig& in) {
              toml::array order;
              for (const auto& filter : in.filters) {
                if (!filter.name.empty()) {
                  order.push_back(filter.name);
                }
              }
              if (!order.empty()) {
                tbl.insert_or_assign("filter_order", std::move(order));
              }
            }
        ),
        namedMap<NotificationConfig, NotificationFilterConfig>(
            &NotificationConfig::filters, "filter", notificationFilterSchema(),
            [](NotificationFilterConfig& filter, std::string_view name) { filter.name = std::string(name); },
            [](const NotificationFilterConfig& filter) { return filter.name; }
        ),
        custom<NotificationConfig>(
            "",
            [](const toml::table& tbl, NotificationConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (const auto* arr = tbl["allowed_urgencies"].as_array()) {
                const std::vector<std::string> global = normalizeFilterAllowedUrgencyStrings(
                    readStringArray(*arr, joinPath(parentPath, "allowed_urgencies"), diag)
                );
                if (!global.empty()) {
                  for (auto& filter : out.filters) {
                    if (filter.allowedUrgencies.empty()) {
                      filter.allowedUrgencies = global;
                    }
                  }
                }
              } else {
                warnWrongType(tbl, "allowed_urgencies", parentPath, diag, "an array of strings");
              }

              const auto* orderArr = tbl["filter_order"].as_array();
              if (orderArr == nullptr) {
                warnWrongType(tbl, "filter_order", parentPath, diag, "an array of strings");
              }
              if (orderArr == nullptr || out.filters.empty()) {
                normalizeNotificationFilterNames(out.filters);
                return;
              }

              std::unordered_map<std::string, NotificationFilterConfig> byName;
              byName.reserve(out.filters.size());
              for (auto& filter : out.filters) {
                if (!filter.name.empty()) {
                  byName.emplace(filter.name, std::move(filter));
                }
              }

              std::vector<NotificationFilterConfig> ordered;
              ordered.reserve(byName.size());
              std::unordered_set<std::string> placed;
              for (const auto& name : readStringArray(*orderArr, joinPath(parentPath, "filter_order"), diag)) {
                const auto it = byName.find(name);
                if (it == byName.end()) {
                  continue;
                }
                ordered.push_back(std::move(it->second));
                placed.insert(name);
              }
              for (auto& [name, filter] : byName) {
                if (!placed.contains(name)) {
                  ordered.push_back(std::move(filter));
                }
              }
              out.filters = std::move(ordered);
              normalizeNotificationFilterNames(out.filters);
            },
            [](toml::table&, const NotificationConfig&) {}
        ),
    };
    return s;
  }

} // namespace noctalia::config::schema
