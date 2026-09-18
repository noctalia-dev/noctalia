#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace default_apps {

  enum class Role : std::uint8_t {
    WebBrowser,
    DocumentViewer,
    TextEditor,
    MailClient,
    MusicPlayer,
    VideoPlayer,
    PhotoViewer,
  };

  struct App {
    // Desktop-file id (e.g. "firefox.desktop").
    std::string id;
    std::string name;
    std::string description;
    // Icon-theme name (e.g. "firefox"), empty when the app exposes none.
    std::string iconName;
  };

  // MIME/content types each role is bound to, in priority order.
  const std::vector<std::string>& mimeTypes(Role role);

  // Installed applications that can handle the role, deduplicated by id.
  [[nodiscard]] std::vector<App> candidates(Role role);

  // Id of the current default handler for the role, read from the user's single
  // mimeapps.list. Other mimeapps files are ignored, so this is empty when that
  // file does not exist or does not name the role's types.
  [[nodiscard]] std::optional<std::string> currentDefault(Role role);

  // Writes the selection to the user's single mimeapps.list for every mime type of the
  // role, creating that file on first write. Other mimeapps files are never touched.
  // Best effort: returns true if at least one association was written.
  [[nodiscard]] bool setDefault(Role role, std::string_view appId);

} // namespace default_apps
