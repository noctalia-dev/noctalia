#include "system/default_apps.h"

#include "core/log.h"
#include "system/desktop_entry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("default_apps");

  // Single source of truth for what each role is bound to: the mime/scheme types
  // that define the role and target default lookups/writes, and the desktop
  // categories the fallback recognizes. Indexed by Role, so the enum order must
  // match this array order.
  struct RoleSpec {
    std::vector<std::string> mimeTypes;
    std::vector<std::string> categories;
  };

  constexpr std::size_t kRoleCount = 7;
  static_assert(static_cast<std::size_t>(default_apps::Role::PhotoViewer) + 1 == kRoleCount);

  const RoleSpec& roleSpec(default_apps::Role role) {
    static const std::array<RoleSpec, kRoleCount> kSpecs{{
        {{"x-scheme-handler/http", "x-scheme-handler/https"}, {"webbrowser"}},
        {{"application/pdf"}, {"viewer", "office"}},
        {{"text/plain"}, {"texteditor", "development"}},
        {{"x-scheme-handler/mailto"}, {"email"}},
        {{"audio/mpeg", "audio/ogg", "audio/flac", "audio/x-wav", "audio/x-m4a", "audio/vorbis"}, {"audio", "player"}},
        {{"video/mp4", "video/x-matroska", "video/webm", "video/quicktime", "video/x-msvideo", "video/x-ms-wmv"},
         {"video", "player"}},
        {{"image/jpeg", "image/png", "image/gif", "image/webp", "image/bmp", "image/tiff"},
         {"graphics", "photography"}},
    }};
    const auto index = static_cast<std::size_t>(role);
    return index < kSpecs.size() ? kSpecs[index] : kSpecs.front();
  }

  // Desktop entry id in the form GIO reports (e.g. "kde-kate.desktop"): the path
  // relative to the applications directory with path separators replaced by '-'
  // and the ".desktop" suffix kept. Keeps default lookups and picker options in
  // agreement for apps installed in applications subdirectories.
  std::string canonicalDesktopId(const std::string& path) {
    const std::string_view applicationsDir("/applications/");
    const std::size_t anchor = path.rfind(applicationsDir);
    const std::string_view relative = anchor == std::string::npos
        ? std::string_view(path)
        : std::string_view(path).substr(anchor + applicationsDir.size());
    std::string id(relative);
    std::ranges::replace(id, '/', '-');
    return id;
  }

  bool appSupportsMime(const DesktopEntry& entry, std::string_view mime) {
    return std::ranges::contains(entry.mimeTypes, mime);
  }

  default_apps::App appFrom(const DesktopEntry& entry) {
    return default_apps::App{
        .id = canonicalDesktopId(entry.path),
        .name = entry.name,
        .description = entry.genericName.empty() ? entry.comment : entry.genericName,
        .iconName = entry.icon,
    };
  }

  // Owns a g_app_info_get_all() list for its scope; frees the entries (which the list
  // holds a reference to) when destroyed.
  const auto freeAppInfos = [](GList* list) {
    if (list != nullptr) {
      g_list_free_full(list, g_object_unref);
    }
  };
  using AppInfoList = std::unique_ptr<GList, decltype(freeAppInfos)>;

  // Exact ';'-delimited category match. Categories like the freedesktop "AudioVideo"
  // catch-all are spelled out explicitly per role instead of using loose substring
  // matching, which could pull in unrelated entries (e.g. a "utility" terminal editor).
  bool categoryMatches(std::string_view categories, const std::vector<std::string>& wanted) {
    for (const std::string_view category : wanted) {
      std::size_t start = 0;
      while (start < categories.size()) {
        const std::size_t end = categories.find(';', start);
        const std::size_t length = end == std::string_view::npos ? categories.size() - start : end - start;
        if (categories.substr(start, length) == category) {
          return true;
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
    }
    return false;
  }

  // Fallback for apps that do not register a MIME/scheme association but ship a
  // recognizable desktop category. Only used when no entry passes the MIME gate.
  std::vector<default_apps::App> categoryFallback(default_apps::Role role) {
    const std::vector<std::string>& categories = roleSpec(role).categories;

    std::vector<default_apps::App> apps;
    for (const auto& entry : desktopEntries()) {
      if (entry.noDisplay || entry.hidden || entry.terminal) {
        continue;
      }
      if (categoryMatches(entry.categoriesLower, categories)) {
        apps.push_back(appFrom(entry));
      }
    }
    return apps;
  }

} // namespace

namespace default_apps {

  const std::vector<std::string>& mimeTypes(Role role) { return roleSpec(role).mimeTypes; }

  std::vector<App> candidates(Role role) {
    const std::vector<std::string>& mimes = roleSpec(role).mimeTypes;
    const std::vector<std::string>& categories = roleSpec(role).categories;

    std::vector<App> apps;
    for (const auto& entry : desktopEntries()) {
      if (entry.noDisplay || entry.hidden || entry.terminal) {
        continue;
      }
      const bool supportsRoleMime =
          std::ranges::any_of(mimes, [&entry](const std::string& mime) { return appSupportsMime(entry, mime); });
      if (!supportsRoleMime) {
        continue;
      }
      // The desktop category is the intent signal: it keeps apps that merely
      // declare a MIME type (browsers, editors, video editors) out of roles they
      // do not belong to even when the MIME gate alone would admit them.
      if (!categoryMatches(entry.categoriesLower, categories)) {
        continue;
      }
      apps.push_back(appFrom(entry));
    }

    if (apps.empty()) {
      apps = categoryFallback(role);
    }
    return apps;
  }

  // Noctalia treats the user's single mimeapps.list as the only source of truth: the
  // per-desktop, system and data-dir mimeapps files are ignored on read, matching the
  // write path that GIO targets at this same file.
  std::optional<std::string> currentDefault(Role role) {
    gchar* path = g_build_filename(g_get_user_config_dir(), "mimeapps.list", nullptr);
    GKeyFile* keyFile = g_key_file_new();
    const bool loaded = g_key_file_load_from_file(keyFile, path, G_KEY_FILE_NONE, nullptr) == TRUE;
    g_free(path);
    if (!loaded) {
      g_key_file_free(keyFile);
      return std::nullopt;
    }

    std::optional<std::string> result;
    for (const auto& mime : roleSpec(role).mimeTypes) {
      gchar* value = g_key_file_get_string(keyFile, "Default Applications", mime.c_str(), nullptr);
      if (value != nullptr && value[0] != '\0') {
        result = value;
        g_free(value);
        break;
      }
      g_free(value);
    }
    g_key_file_free(keyFile);
    return result;
  }

  bool setDefault(Role role, std::string_view appId) {
    if (appId.empty()) {
      return false;
    }
    const std::string idText(appId);

    GDesktopAppInfo* desktopInfo = g_desktop_app_info_new(idText.c_str());
    GAppInfo* info = desktopInfo != nullptr ? G_APP_INFO(desktopInfo) : nullptr;
    if (info == nullptr) {
      // The candidate is ref'd below so AppInfoList can stay scoped to this block;
      // otherwise the subdirectory-id fallback would leave info pointing at an entry
      // the list unrefs (and frees) when it is destroyed.
      AppInfoList allApps(g_app_info_get_all());
      if (allApps.get() != nullptr) {
        for (GList* it = allApps.get(); it != nullptr; it = it->next) {
          auto* candidate = static_cast<GAppInfo*>(it->data);
          if (const char* candidateId = g_app_info_get_id(candidate); candidateId != nullptr && idText == candidateId) {
            info = static_cast<GAppInfo*>(g_object_ref(candidate));
            break;
          }
        }
      }
    }
    if (info == nullptr) {
      kLog.warn("no installed application matched '{}'", idText);
      return false;
    }

    // Best effort: GIO writes each association independently, so a failure partway
    // through can leave a subset applied. Report success if any type was written so
    // callers still refresh and show the state that actually reached disk.
    bool anySucceeded = false;
    for (const auto& mime : roleSpec(role).mimeTypes) {
      GError* error = nullptr;
      if (g_app_info_set_as_default_for_type(info, mime.c_str(), &error) != TRUE) {
        kLog.warn(
            "failed to set '{}' as default for '{}': {}", idText, mime,
            error != nullptr ? error->message : "unknown error"
        );
        if (error != nullptr) {
          g_error_free(error);
        }
      } else {
        anySucceeded = true;
      }
    }
    g_object_unref(info);
    return anySucceeded;
  }

} // namespace default_apps
