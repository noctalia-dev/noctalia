#include "theme/kde_color_scheme.h"

#include "config/atomic_file.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr const char* kKdeGlobalsSignalPath = "/KGlobalSettings";
    constexpr const char* kKdeGlobalsSignalInterface = "org.kde.KGlobalSettings";
    constexpr const char* kKdeGlobalsSignalName = "notifyChange";

    struct KdeConfigKey {
      std::string name;
      std::string value;
      std::size_t lineIndex = 0;
    };

    struct KdeConfigSection {
      std::string name;
      std::string header;
      std::vector<std::string> lines;
      std::vector<KdeConfigKey> keys;
      std::unordered_map<std::string, std::size_t> keyIndices;
    };

    struct KdeConfigDocument {
      std::vector<std::string> prefixLines;
      std::vector<KdeConfigSection> sections;
      std::unordered_map<std::string, std::size_t> sectionIndices;
    };

    void setError(std::string* error, std::string message) {
      if (error != nullptr) {
        *error = std::move(message);
      }
    }

    bool isCommentOrBlank(std::string_view line) {
      const std::string trimmed = StringUtils::trim(line);
      return trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';';
    }

    bool
    parseKdeConfig(const std::filesystem::path& path, bool required, KdeConfigDocument& document, std::string* error) {
      std::error_code statusError;
      const bool exists = std::filesystem::exists(path, statusError);
      if (statusError) {
        setError(error, "failed to inspect " + path.string() + ": " + statusError.message());
        return false;
      }
      if (!exists) {
        if (!required) {
          return true;
        }
        setError(error, "failed to load " + path.string() + ": file does not exist");
        return false;
      }

      std::ifstream input(path);
      if (!input) {
        setError(error, "failed to load " + path.string());
        return false;
      }

      KdeConfigSection* currentSection = nullptr;
      std::string line;
      std::size_t lineNumber = 0;
      while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        const std::string trimmed = StringUtils::trim(line);
        if (isCommentOrBlank(trimmed)) {
          (currentSection != nullptr ? currentSection->lines : document.prefixLines).push_back(std::move(line));
          continue;
        }

        if (trimmed.front() == '[') {
          if (trimmed.size() < 3 || trimmed.back() != ']') {
            setError(error, "invalid group at " + path.string() + ':' + std::to_string(lineNumber));
            return false;
          }
          const std::string groupName = trimmed.substr(1, trimmed.size() - 2);
          if (groupName.empty() || document.sectionIndices.contains(groupName)) {
            setError(error, "duplicate or empty group at " + path.string() + ':' + std::to_string(lineNumber));
            return false;
          }
          document.sectionIndices.emplace(groupName, document.sections.size());
          document.sections.push_back(KdeConfigSection{.name = groupName, .header = std::move(line)});
          currentSection = &document.sections.back();
          continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (currentSection == nullptr || equals == std::string::npos) {
          setError(error, "invalid key at " + path.string() + ':' + std::to_string(lineNumber));
          return false;
        }
        const std::string keyName = StringUtils::trim(std::string_view(trimmed).substr(0, equals));
        if (keyName.empty() || currentSection->keyIndices.contains(keyName)) {
          setError(error, "duplicate or empty key at " + path.string() + ':' + std::to_string(lineNumber));
          return false;
        }
        const std::string value = StringUtils::trim(std::string_view(trimmed).substr(equals + 1));
        const std::size_t lineIndex = currentSection->lines.size();
        currentSection->lines.push_back(std::move(line));
        currentSection->keyIndices.emplace(keyName, currentSection->keys.size());
        currentSection->keys.push_back(KdeConfigKey{.name = keyName, .value = value, .lineIndex = lineIndex});
      }

      if (!input.eof()) {
        setError(error, "failed to read " + path.string());
        return false;
      }
      return true;
    }

    void appendKey(KdeConfigSection& section, const KdeConfigKey& key) {
      std::size_t insertAt = section.lines.size();
      while (insertAt > 0 && isCommentOrBlank(section.lines[insertAt - 1])) {
        --insertAt;
      }
      section.lines.insert(section.lines.begin() + static_cast<std::ptrdiff_t>(insertAt), key.name + '=' + key.value);
    }

    void mergeKdeConfig(KdeConfigDocument& destination, const KdeConfigDocument& source) {
      for (const KdeConfigSection& sourceSection : source.sections) {
        const auto destinationSectionIt = destination.sectionIndices.find(sourceSection.name);
        if (destinationSectionIt == destination.sectionIndices.end()) {
          KdeConfigSection newSection{.name = sourceSection.name, .header = '[' + sourceSection.name + ']'};
          for (const KdeConfigKey& key : sourceSection.keys) {
            appendKey(newSection, key);
          }
          destination.sectionIndices.emplace(newSection.name, destination.sections.size());
          destination.sections.push_back(std::move(newSection));
          continue;
        }

        KdeConfigSection& destinationSection = destination.sections[destinationSectionIt->second];
        for (const KdeConfigKey& sourceKey : sourceSection.keys) {
          const auto destinationKeyIt = destinationSection.keyIndices.find(sourceKey.name);
          if (destinationKeyIt == destinationSection.keyIndices.end()) {
            appendKey(destinationSection, sourceKey);
            continue;
          }
          const KdeConfigKey& destinationKey = destinationSection.keys[destinationKeyIt->second];
          destinationSection.lines[destinationKey.lineIndex] = sourceKey.name + '=' + sourceKey.value;
        }
      }
    }

    std::string serializeKdeConfig(const KdeConfigDocument& document) {
      std::ostringstream output;
      for (const std::string& line : document.prefixLines) {
        output << line << '\n';
      }
      for (std::size_t sectionIndex = 0; sectionIndex < document.sections.size(); ++sectionIndex) {
        if (sectionIndex > 0 && output.tellp() > 0) {
          const KdeConfigSection& previousSection = document.sections[sectionIndex - 1];
          if (previousSection.lines.empty() || !previousSection.lines.back().empty()) {
            output << '\n';
          }
        }
        const KdeConfigSection& section = document.sections[sectionIndex];
        output << section.header << '\n';
        for (const std::string& line : section.lines) {
          output << line << '\n';
        }
      }
      return output.str();
    }

    std::optional<std::filesystem::perms> existingPermissions(const std::filesystem::path& path) {
      std::error_code statusError;
      const auto status = std::filesystem::status(path, statusError);
      if (statusError || !std::filesystem::exists(status)) {
        return std::nullopt;
      }
      return status.permissions();
    }

    std::filesystem::path kdeGlobalsPath() { return FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/kdeglobals"); }

    std::string notifyKdeColorSchemeChanged() {
      try {
        auto connection = sdbus::createSessionBusConnection();
        auto object = sdbus::createObject(*connection, sdbus::ObjectPath{kKdeGlobalsSignalPath});
        object->emitSignal(kKdeGlobalsSignalName)
            .onInterface(kKdeGlobalsSignalInterface)
            .withArguments(std::int32_t{0}, std::int32_t{0});
      } catch (const sdbus::Error& error) {
        return error.what();
      }
      return {};
    }

  } // namespace

  bool mergeKdeColorScheme(
      const std::filesystem::path& schemePath, const std::filesystem::path& kdeGlobalsPath, std::string* error
  ) {
    KdeConfigDocument scheme;
    if (!parseKdeConfig(schemePath, true, scheme, error)) {
      return false;
    }
    if (scheme.sections.empty()) {
      setError(error, "color scheme contains no groups: " + schemePath.string());
      return false;
    }

    KdeConfigDocument kdeGlobals;
    if (!parseKdeConfig(kdeGlobalsPath, false, kdeGlobals, error)) {
      return false;
    }
    mergeKdeConfig(kdeGlobals, scheme);
    const std::string serialized = serializeKdeConfig(kdeGlobals);

    if (!writeTextFileAtomic(kdeGlobalsPath, serialized, existingPermissions(kdeGlobalsPath))) {
      setError(error, "failed to write " + kdeGlobalsPath.string());
      return false;
    }
    return true;
  }

  KdeColorSchemeApplyResult applyKdeColorScheme(const std::filesystem::path& schemePath) {
    KdeColorSchemeApplyResult result;
    const std::filesystem::path globalsPath = kdeGlobalsPath();
    if (!globalsPath.is_absolute()) {
      result.error = "failed to resolve $XDG_CONFIG_HOME/kdeglobals";
      return result;
    }
    if (!mergeKdeColorScheme(schemePath, globalsPath, &result.error)) {
      return result;
    }

    result.success = true;
    result.notificationError = notifyKdeColorSchemeChanged();
    return result;
  }

} // namespace noctalia::theme
