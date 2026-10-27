#include "theme/builtin_templates.h"

#include "core/files/resource_paths.h"
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
      if (const auto url = info->get_as<std::string>("documentation_url")) {
        entry.documentationUrl = url->get();
      }
      out.push_back(std::move(entry));
    }

    const toml::table* templates = root["templates"].as_table();
    if (templates != nullptr) {
      for (auto& entry : out) {
        const toml::table* tpl = templates->get_as<toml::table>(entry.id);
        if (tpl == nullptr) {
          continue;
        }
        if (const auto opd = tpl->get_as<std::string>("output_path_dynamic")) {
          entry.outputDynamic = true;
          entry.outputPathDynamicCommand = opd->get();
        }
        const toml::node* op = tpl->get("output_path");
        if (op != nullptr) {
          if (const auto str = op->as_string()) {
            entry.outputPaths.push_back(str->get());
          } else if (const auto arr = op->as_array()) {
            for (const auto& item : *arr) {
              if (const auto itemStr = item.as_string()) {
                entry.outputPaths.push_back(itemStr->get());
              }
            }
          }
        }
      }
    }

    // Dynamic output paths (output_path_dynamic) are resolved by the template
    // engine at apply time — the only consumers here (settings tooltip, CLI list)
    // do not need concrete paths, so we do not spawn a shell per template just to
    // populate a tooltip. Such entries keep outputDynamic = true, and the tooltip
    // reports "(output resolved at apply time)".

    std::ranges::sort(out, [](const BuiltinTemplateInfo& lhs, const BuiltinTemplateInfo& rhs) {
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
      t.documentationUrl = std::move(entry.documentationUrl);
      t.outputPaths = std::move(entry.outputPaths);
      t.outputDynamic = entry.outputDynamic;
      out.push_back(std::move(t));
    }
    std::ranges::sort(out, [](const AvailableTemplate& a, const AvailableTemplate& b) {
      if (a.displayName != b.displayName) {
        return a.displayName < b.displayName;
      }
      return a.id < b.id;
    });
    out.erase(
        std::ranges::unique(
            out, [](const AvailableTemplate& a, const AvailableTemplate& b) { return a.id == b.id; }
        ).begin(),
        out.end()
    );
    return out;
  }

} // namespace noctalia::theme
