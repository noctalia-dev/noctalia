#include "config/config_origins.h"

namespace noctalia::config {

  void ConfigOriginIndex::record(const std::filesystem::path& file, const toml::table& tbl) {
    const std::string fileStr = file.string();
    std::string path;
    for (const auto& [key, node] : tbl) {
      path.assign(key.str());
      recordInto(path, node, fileStr);
    }
  }

  void ConfigOriginIndex::recordInto(std::string& path, const toml::node& node, const std::string& file) {
    if (const auto& src = node.source(); src.begin.line != 0) {
      m_entries.insert_or_assign(path, schema::SourceOrigin{file, src.begin.line, src.begin.column});
    }
    const auto* tbl = node.as_table();
    if (tbl == nullptr) {
      return;
    }
    const std::size_t parentSize = path.size();
    for (const auto& [key, child] : *tbl) {
      path.push_back('.');
      path.append(key.str());
      recordInto(path, child, file);
      path.resize(parentSize);
    }
  }

  const schema::SourceOrigin* ConfigOriginIndex::find(std::string_view path) const {
    while (!path.empty()) {
      if (const auto it = m_entries.find(path); it != m_entries.end()) {
        return &it->second;
      }
      const std::size_t dot = path.rfind('.');
      if (dot == std::string_view::npos) {
        return nullptr;
      }
      path.remove_suffix(path.size() - dot);
    }
    return nullptr;
  }

  void ConfigOriginIndex::annotate(schema::Diagnostics& diag) const {
    for (auto& entry : diag.entries) {
      if (entry.origin.valid()) {
        continue;
      }
      if (const schema::SourceOrigin* origin = find(entry.path)) {
        entry.origin = *origin;
      }
    }
  }

  schema::SourceOrigin parseErrorOrigin(const toml::parse_error& error, const std::filesystem::path& file) {
    const auto& src = error.source();
    return schema::SourceOrigin{file.string(), src.begin.line, src.begin.column};
  }

} // namespace noctalia::config
