#include "config/schema/config_schema.h"

#include "config/schema/config_sections.h"
#include "config/schema/engine.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace noctalia::config::schema {

  namespace {
    // Run collectUnknownKeys for `section`'s schema; false if the section name is
    // unknown. The single dispatch from a section name to its schema.
    bool collectUnknownInSection(std::string_view section, const toml::table& tbl, std::vector<std::string>& unknown) {
      if (const SectionSpec* spec = findSection(section); spec != nullptr) {
        spec->collectUnknown(tbl, unknown);
        return true;
      }
      // Schema-backed but not plain sections: their read/write shape is bespoke, so
      // they are custom root keys rather than SectionSpec rows.
      if (section == "desktop_widgets") {
        collectUnknownKeys(tbl, desktopWidgetsSchema(), section, unknown);
        return true;
      }
      if (section == "lockscreen_widgets") {
        collectUnknownKeys(tbl, lockscreenWidgetsSchema(), section, unknown);
        return true;
      }
      return false;
    }

    // Nested table mirroring path[from..] with a dummy leaf, so collectUnknownKeys
    // can report whether the deepest key is recognized.
    toml::table nestedFromPath(const std::vector<std::string>& path, std::size_t from) {
      toml::table root;
      toml::table* cur = &root;
      for (std::size_t i = from; i + 1 < path.size(); ++i) {
        cur->insert_or_assign(path[i], toml::table{});
        cur = cur->get(path[i])->as_table();
      }
      cur->insert_or_assign(path.back(), 0); // dummy leaf
      return root;
    }

    bool isKnownDesktopWidgetPath(const std::vector<std::string>& path) {
      if (path.size() == 2 && path[1] == "widget_order") {
        return true;
      }
      if (path.size() < 2 || path[1] != "widget") {
        return false;
      }
      if (path.size() <= 3) {
        return true;
      }
      static const std::unordered_set<std::string> kWidgetKeys = {
          "id",        "type",       "output",   "cx",     "cy",     "placement_width", "placement_height",
          "box_width", "box_height", "rotation", "flip_x", "flip_y", "enabled",         "settings",
      };
      if (!kWidgetKeys.contains(path[3])) {
        return false;
      }
      return path.size() == 4 || path[3] == "settings";
    }
  } // namespace

  bool isKnownConfigPath(const std::vector<std::string>& path) {
    if (path.empty()) {
      return false;
    }
    const std::string& section = path[0];

    // Bar lives at the config root (named bars + monitor overrides), not a section
    // schema. {"bar"} / {"bar",name} / {"bar",name,"monitor"[,match]} are container
    // levels; deeper keys validate against the bar field schemas.
    if (section == "bar") {
      if (path.size() <= 2) {
        return true;
      }
      if (path[2] == "monitor") {
        if (path.size() <= 4) {
          return true;
        }
        std::vector<std::string> unknown;
        collectUnknownKeys(nestedFromPath(path, 4), barMonitorOverrideSchema(), "bar", unknown);
        return unknown.empty();
      }
      if (path.size() == 3 && (path[2] == "position" || path[2] == "name")) {
        return true; // emitted/keyed outside barFieldsSchema
      }
      std::vector<std::string> unknown;
      collectUnknownKeys(nestedFromPath(path, 2), barFieldsSchema(), "bar", unknown);
      return unknown.empty();
    }

    if (section == "desktop_widgets" && isKnownDesktopWidgetPath(path)) {
      return true;
    }

    // [plugin_settings."author/plugin"].<key>, open schema; keys validate against
    // the manifest in config_validate's validatePluginSettings, not here.
    if (section == "plugin_settings") {
      return path.size() <= 3;
    }

    if (path.size() < 2) {
      return false; // a bare section is not a setting path
    }
    std::vector<std::string> unknown;
    if (!collectUnknownInSection(section, nestedFromPath(path, 1), unknown)) {
      return false; // unknown section
    }
    return unknown.empty();
  }

} // namespace noctalia::config::schema
