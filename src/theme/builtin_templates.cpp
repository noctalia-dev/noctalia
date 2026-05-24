#include "theme/builtin_templates.h"

#include "core/resource_paths.h"
#include "core/toml.h" // IWYU pragma: keep

#include <algorithm>
#include <filesystem>

namespace noctalia::theme {

  std::vector<BuiltinTemplateInfo> loadBuiltinTemplateInfo(std::string* err) {
    const std::filesystem::path configPath = paths::assetPath("templates/builtin.toml");
    toml::table root;
    try {
      root = toml::parse_file(configPath.string());
    } catch (const toml::parse_error& e) {
      if (err != nullptr) {
        *err = e.description();
      }
      return {};
    }

    std::vector<BuiltinTemplateInfo> out;
    const toml::table* catalog = root["catalog"].as_table();
    if (catalog == nullptr) {
      return out;
    }

    for (const auto& [idNode, node] : *catalog) {
      const toml::table* info = node.as_table();
      if (info == nullptr) {
        continue;
      }
      BuiltinTemplateInfo entry;
      entry.id = std::string(idNode.str());
      if (const auto name = info->get_as<std::string>("name")) {
        entry.name = name->get();
      }
      if (const auto category = info->get_as<std::string>("category")) {
        entry.category = category->get();
      }
      out.push_back(std::move(entry));
    }

    std::sort(out.begin(), out.end(), [](const BuiltinTemplateInfo& lhs, const BuiltinTemplateInfo& rhs) {
      if (lhs.category != rhs.category) {
        return lhs.category < rhs.category;
      }
      return lhs.id < rhs.id;
    });
    return out;
  }

  std::vector<AvailableTemplate> availableTemplates() {
    auto entries = loadBuiltinTemplateInfo();
    std::vector<AvailableTemplate> out;
    out.reserve(entries.size());
    for (auto& entry : entries) {
      AvailableTemplate t;
      t.id = std::move(entry.id);
      t.displayName = entry.name.empty() ? t.id : std::move(entry.name);
      t.category = std::move(entry.category);
      out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end(), [](const AvailableTemplate& a, const AvailableTemplate& b) {
      if (a.displayName != b.displayName) {
        return a.displayName < b.displayName;
      }
      return a.id < b.id;
    });
    out.erase(
        std::unique(
            out.begin(), out.end(), [](const AvailableTemplate& a, const AvailableTemplate& b) { return a.id == b.id; }
        ),
        out.end()
    );
    return out;
  }

} // namespace noctalia::theme
